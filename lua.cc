/*
 * Copyright (C) 2019 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lua.hh"
#include "exceptions/exceptions.hh"
#include "concrete_types.hh"
#include <lua.hpp>

using namespace seastar;

static logging::logger lua_logger("lua");

namespace {
struct alloc_state {
    size_t allocated = 0;
    size_t max;
    size_t max_contiguous;
    alloc_state(size_t max, size_t max_contiguous)
        : max(max)
        , max_contiguous(max_contiguous) {
        // The max and max_contiguous limits are responsible for avoiding overflows.
        assert(max + max_contiguous >= max);
    }
};

struct lua_closer {
    void operator()(lua_State* l) {
        lua_close(l);
    }
};

class lua_slice_state {
    std::unique_ptr<alloc_state> a_state;
    std::unique_ptr<lua_State, lua_closer> _l;
public:
    lua_slice_state(std::unique_ptr<alloc_state> a_state, std::unique_ptr<lua_State, lua_closer> l)
        : a_state(std::move(a_state))
        , _l(std::move(l)) {}
    operator lua_State*() { return _l.get(); }
};
}

static void* lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* s = reinterpret_cast<alloc_state*>(ud);

    // avoid realloc(nullptr, 0), which would allocate.
    if (nsize == 0 && ptr == nullptr) {
        return nullptr;
    }

    if (nsize > s->max_contiguous) {
        return nullptr;
    }

    size_t next = s->allocated + nsize;

    // The max and max_contiguous limits should be small enough to avoid overflows.
    assert(next >= s->allocated);

    if (ptr) {
        next -= osize;
    }

    if (next > s->max) {
        lua_logger.info("allocation failed. alread allocated = {}, next total = {}, max = {}", s->allocated, next, s->max);
        return nullptr;
    }

    // FIXME: Given that we have osize, we can probably do better when
    // SEASTAR_DEFAULT_ALLOCATOR is false
    void* ret = realloc(ptr, nsize);

    if (nsize == 0 || ret != nullptr) {
        s->allocated = next;
    }
    return ret;
}

static const luaL_Reg loadedlibs[] = {
    {"_G", luaopen_base},
    {LUA_COLIBNAME, luaopen_string},
    {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table},
    {NULL, NULL},
};

static void debug_hook(lua_State* l, lua_Debug* ar) {
    if (!need_preempt()) {
        return;
    }
    // The lua manual says that only count and line events can yield. Of those we only use count.
    if (ar->event != LUA_HOOKCOUNT) {
        // Set the hook to stop at the very next lua instruction, where we will be able to yield.
        lua_sethook(l, debug_hook, LUA_MASKCOUNT, 1);
        return;
    }
    if (lua_yield(l, 0)) {
        assert(0 && "lua_yield failed");
    }
}

static lua_slice_state new_lua(const lua::runtime_config& cfg) {
    auto a_state = std::make_unique<alloc_state>(cfg.max_bytes, cfg.max_contiguous);
    std::unique_ptr<lua_State, lua_closer> l{lua_newstate(lua_alloc, a_state.get())};
    if (!l) {
        throw std::runtime_error("could not create lua state");
    }
    return lua_slice_state{std::move(a_state), std::move(l)};
}

static int string_writer(lua_State*, const void* p, size_t size, void* data) {
    luaL_addlstring(reinterpret_cast<luaL_Buffer*>(data), reinterpret_cast<const char*>(p), size);
    return 0;
}

static int compile_l(lua_State* l) {
    const auto& script = *reinterpret_cast<sstring*>(lua_touserdata(l, 1));
    luaL_Buffer buf;
    luaL_buffinit(l, &buf);

    if (luaL_loadbufferx(l, script.c_str(), script.size(), "<internal>", "t")) {
        lua_error(l);
    }
    if (lua_dump(l, string_writer, &buf, true)) {
        luaL_error(l, "lua_dump failed");
    }
    luaL_pushresult(&buf);

    return 1;
}

sstring lua::compile(const runtime_config& cfg, const std::vector<sstring>& arg_names, sstring script) {
    if (!arg_names.empty()) {
        // In Lua, all chunks are compiled to vararg functions. To use
        // the UDF argument names, we start the chunk with "local
        // arg1,arg2,etc = ...", which captures the arguments when the
        // chunk is called.
        std::ostringstream os;
        os << "local ";
        for (int i = 0, n = arg_names.size(); i < n; ++i) {
            if (i != 0) {
                os << ",";
            }
            os << arg_names[i];
        }
        os << " = ...;\n" << script;
        script = os.str();
    }
    lua_slice_state l = new_lua(cfg);

    // Run the load from lua_pcall so we don't have to handle longjmp.
    lua_pushcfunction(l, compile_l);
    lua_pushlightuserdata(l, &script);
    if (lua_pcall(l, 1, 1, 0)) {
        throw exceptions::invalid_request_exception(std::string("could not compile: ") + lua_tostring(l, -1));
    }

    size_t len;
    const char* p = lua_tolstring(l, -1, &len);
    return sstring(p, len);
}

static int load_script_l(lua_State* l) {
    const auto& bitcode = *reinterpret_cast<lua::bitcode_view*>(lua_touserdata(l, 1));
    const auto& binary = bitcode.bitcode;

    for (const luaL_Reg* lib = loadedlibs; lib->func; lib++) {
        luaL_requiref(l, lib->name, lib->func, 1);
        lua_pop(l, 1);
    }

    if (luaL_loadbufferx(l, binary.data(), binary.size(), "<internal>", "b")) {
        lua_error(l);
    }

    return 1;
}

static lua_slice_state load_script(const lua::runtime_config& cfg, lua::bitcode_view binary) {
    lua_slice_state l = new_lua(cfg);

    // Run the initialization from lua_pcall so we don't have to
    // handle longjmp. We know that a new state has a few reserved
    // stack slots and the following push calls don't allocate.
    lua_pushcfunction(l, load_script_l);
    lua_pushlightuserdata(l, &binary);
    if (lua_pcall(l, 1, 1, 0)) {
        throw std::runtime_error(std::string("could not initiate: ") + lua_tostring(l, -1));
    }

    return l;
}

using millisecond = std::chrono::duration<double, std::milli>;
static auto now() { return std::chrono::system_clock::now(); }

static data_value convert_from_lua(lua_slice_state &l, const data_type& type) {
    int isnum;
    int64_t ret = lua_tointegerx(l, -1, &isnum);
    if (!isnum) {
        throw exceptions::invalid_request_exception("value is not an integer");
    }
    return int32_t(ret);
}

static bytes convert_return(lua_slice_state &l, const data_type& return_type) {
    int num_return_vals = lua_gettop(l);
    if (num_return_vals != 1) {
        throw exceptions::invalid_request_exception(
            format("{} values returned, expected {}", num_return_vals, 1));
    }

    // FIXME: It should be possible to avoid creating the data_value,
    // or even better, change the function::execute interface to
    // return a data_value instead of bytes_opt.
    return convert_from_lua(l, return_type).serialize();
}

namespace {
struct to_lua_visitor {
    lua_slice_state& l;

    void operator()(const varint_type_impl& t, const emptyable<boost::multiprecision::cpp_int>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const decimal_type_impl& t, const emptyable<big_decimal>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const counter_type_impl& t, const void* v) {
        // This is unreachable since deserialize_visitor for
        // counter_type_impl return a long.
        abort();
    }

    void operator()(const empty_type_impl& t, const void* v) {
        // This is unreachable since empty types are not user visible.
        abort();
    }

    void operator()(const reversed_type_impl& t, const void* v) {
        // This is unreachable since reversed_type_impl is used only
        // in the tables. The function gets the underlying type.
        abort();
    }

    void operator()(const map_type_impl& t, const std::vector<std::pair<data_value, data_value>>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const user_type_impl& t, const std::vector<data_value>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const set_type_impl& t, const std::vector<data_value>* v) {
        assert(0 && "not implemented");
    }

    template <typename T>
    void operator()(const concrete_type<std::vector<data_value>, T>& t, const std::vector<data_value>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const boolean_type_impl& t, const emptyable<bool>* v) {
        assert(0 && "not implemented");
    }

    template <typename T>
    void operator()(const floating_type_impl<T>& t, const emptyable<T>* v) {
        assert(0 && "not implemented");
    }

    template <typename T>
    void operator()(const integer_type_impl<T>& t, const emptyable<T>* v) {
        // Integers are converted to 64 bits
        lua_pushinteger(l, *v);
    }

    void operator()(const bytes_type_impl& t, const bytes* v) {
        assert(0 && "not implemented");
    }

    void operator()(const string_type_impl& t, const sstring* v) {
        assert(0 && "not implemented");
    }

    void operator()(const time_type_impl& t, const emptyable<int64_t>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const timestamp_date_base_class& t, const timestamp_date_base_class::native_type* v) {
        assert(0 && "not implemented");
    }

    void operator()(const simple_date_type_impl& t, const emptyable<uint32_t>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const duration_type_impl& t, const emptyable<cql_duration>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const inet_addr_type_impl& t, const emptyable<seastar::net::inet_address>* v) {
        assert(0 && "not implemented");
    }

    void operator()(const concrete_type<utils::UUID>&, const emptyable<utils::UUID>* v) {
        assert(0 && "not implemented");
    }
};
}

static void push_argument(lua_slice_state& l, const data_value& arg) {
    if (arg.is_null()) {
        lua_pushnil(l);
        return;
    }
    ::visit(arg, to_lua_visitor{l});
}

lua::runtime_config lua::make_runtime_config(const db::config& config) {
    utils::updateable_value<unsigned> max_bytes(config.user_defined_function_allocation_limit_bytes);
    utils::updateable_value<unsigned> max_contiguous(config.user_defined_function_contiguous_allocation_limit_bytes());
    utils::updateable_value<unsigned> timeout_in_ms(config.user_defined_function_time_limit_ms());

    return lua::runtime_config{std::move(timeout_in_ms), std::move(max_bytes), std::move(max_contiguous)};
}

// run the script for at most max_instructions
future<bytes> lua::run_script(lua::bitcode_view bitcode, const std::vector<data_value>& values, data_type return_type, const lua::runtime_config& cfg) {
    lua_slice_state l = load_script(cfg, bitcode);
    unsigned nargs = values.size();
    if (!lua_checkstack(l, nargs)) {
        throw std::runtime_error("could push args to the stack");
    }
    for (const data_value& arg : values) {
        push_argument(l, arg);
    }

    // We don't update the timeout once we start executing the function
    using millisecond = std::chrono::duration<double, std::milli>;
    using duration = std::chrono::system_clock::duration;
    duration elapsed{0};
    duration timeout = std::chrono::duration_cast<duration>(millisecond(cfg.timeout_in_ms));
    return repeat_until_value([l = std::move(l), elapsed, return_type, nargs, timeout = std::move(timeout)] () mutable {
        // Set the hook before resuming. We have to do it here since the hook can reset itself
        // if it detects we are spending too much time in C.
        // The hook will be called after 1000 instructions.
        lua_sethook(l, debug_hook, LUA_MASKCALL | LUA_MASKCOUNT, 1000);
        auto start = ::now();
        switch (lua_resume(l, nullptr, nargs)) {
        case LUA_OK:
            return make_ready_future<bytes_opt>(convert_return(l, return_type));
        case LUA_YIELD: {
            nargs = 0;
            elapsed += ::now() - start;
            if (elapsed > timeout) {
                millisecond ms = elapsed;
                throw exceptions::invalid_request_exception(format("lua execution timeout: {}ms elapsed", ms.count()));
            }
            return make_ready_future<bytes_opt>(bytes_opt());
        }
        default:
            throw exceptions::invalid_request_exception(std::string("lua execution failed: ") +
                                                        lua_tostring(l, -1));
        }
    });
}
