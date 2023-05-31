%
% This file is part of AtomVM.
%
% Copyright 2023 Paul Guyot <pguyot@kallisys.net>
%
% Licensed under the Apache License, Version 2.0 (the "License");
% you may not use this file except in compliance with the License.
% You may obtain a copy of the License at
%
%    http://www.apache.org/licenses/LICENSE-2.0
%
% Unless required by applicable law or agreed to in writing, software
% distributed under the License is distributed on an "AS IS" BASIS,
% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
% See the License for the specific language governing permissions and
% limitations under the License.
%
% SPDX-License-Identifier: Apache-2.0 OR LGPL-2.1-or-later
%

-module(test_file).

-export([test/0]).

-include("etest.hrl").

test() ->
    HasSelect = atomvm:platform() =/= emscripten,
    ok = test_basic_file(),
    ok = test_fifo_select(HasSelect),
    ok = test_gc(HasSelect),
    ok.

test_basic_file() ->
    Path = "/tmp/atomvm.tmp." ++ integer_to_list(erlang:system_time(millisecond)),
    {ok, Fd} = atomvm:posix_open(Path, [o_wronly, o_creat], 8#644),
    {ok, 5} = atomvm:posix_write(Fd, <<"Hello">>),
    ok = atomvm:posix_close(Fd),
    {ok, Fd2} = atomvm:posix_open(Path, [o_rdwr]),
    {ok, <<"He">>} = atomvm:posix_read(Fd2, 2),
    {ok, <<"llo">>} = atomvm:posix_read(Fd2, 10),
    eof = atomvm:posix_read(Fd2, 10),
    {ok, 6} = atomvm:posix_write(Fd2, <<" World">>),
    eof = atomvm:posix_read(Fd2, 10),
    ok = atomvm:posix_close(Fd2),
    ok = atomvm:posix_unlink(Path).

test_fifo_select(false) ->
    ok;
test_fifo_select(_HasSelect) ->
    Path = "/tmp/atomvm.tmp." ++ integer_to_list(erlang:system_time(millisecond)),
    ok = atomvm:posix_mkfifo(Path, 8#644),
    {ok, RdFd} = atomvm:posix_open(Path, [o_rdonly]),
    {ok, WrFd} = atomvm:posix_open(Path, [o_wronly]),
    SelectWriteRef = make_ref(),
    ok = atomvm:posix_select_write(WrFd, self(), SelectWriteRef),
    ok =
        receive
            {select, WrFd, SelectWriteRef, ready_output} -> ok;
            M -> {unexpected, M}
        after 200 -> fail
        end,
    ok = atomvm:posix_select_stop(WrFd),
    {ok, 5} = atomvm:posix_write(WrFd, <<"Hello">>),
    SelectReadRef = make_ref(),
    ok = atomvm:posix_select_read(RdFd, self(), SelectReadRef),
    ok =
        receive
            {select, RdFd, SelectReadRef, ready_input} -> ok
        after 200 -> fail
        end,
    {ok, <<"Hello">>} = atomvm:posix_read(RdFd, 10),
    ok = atomvm:posix_select_read(RdFd, self(), SelectReadRef),
    ok =
        receive
            {select, RdFd, SelectReadRef, _} -> fail
        after 200 -> ok
        end,
    {ok, 6} = atomvm:posix_write(WrFd, <<" World">>),
    ok =
        receive
            {select, RdFd, SelectReadRef, ready_input} -> ok;
            M2 -> {unexpected, M2}
        after 200 -> fail
        end,
    ok = atomvm:posix_select_stop(RdFd),
    ok =
        receive
            Message -> {unexpected, Message}
        after 200 -> ok
        end,
    ok = atomvm:posix_close(RdFd),
    ok = atomvm:posix_close(WrFd),
    ok = atomvm:posix_unlink(Path).

% Test is based on the fact that `erlang:memory(binary)` count resources.
test_gc(HasSelect) ->
    Path = "/tmp/atomvm.tmp." ++ integer_to_list(erlang:system_time(millisecond)),
    GCSubPid = spawn(fun() -> gc_loop(Path, undefined) end),
    MemorySize0 = erlang:memory(binary),
    call_gc_loop(GCSubPid, open),
    MemorySize1 = erlang:memory(binary),
    ?ASSERT_TRUE(MemorySize1 > MemorySize0),
    call_gc_loop(GCSubPid, close),
    call_gc_loop(GCSubPid, gc),
    MemorySize2 = erlang:memory(binary),
    ?ASSERT_EQUALS(MemorySize2, MemorySize0),

    call_gc_loop(GCSubPid, open),
    MemorySize3 = erlang:memory(binary),
    ?ASSERT_EQUALS(MemorySize3, MemorySize1),
    call_gc_loop(GCSubPid, open),
    call_gc_loop(GCSubPid, gc),
    MemorySize4 = erlang:memory(binary),
    ?ASSERT_EQUALS(MemorySize4, MemorySize1),
    call_gc_loop(GCSubPid, forget),
    call_gc_loop(GCSubPid, gc),
    MemorySize5 = erlang:memory(binary),
    ?ASSERT_EQUALS(MemorySize5, MemorySize0),

    if
        HasSelect ->
            call_gc_loop(GCSubPid, open),
            MemorySize6 = erlang:memory(binary),
            ?ASSERT_EQUALS(MemorySize6, MemorySize1),
            call_gc_loop(GCSubPid, select_write),
            call_gc_loop(GCSubPid, gc),
            MemorySize7 = erlang:memory(binary),
            ?ASSERT_EQUALS(MemorySize7, MemorySize1),
            call_gc_loop(GCSubPid, select_stop),
            call_gc_loop(GCSubPid, gc),
            MemorySize8 = erlang:memory(binary),
            ?ASSERT_EQUALS(MemorySize8, MemorySize1),
            call_gc_loop(GCSubPid, close),
            call_gc_loop(GCSubPid, gc),
            MemorySize9 = erlang:memory(binary),
            ?ASSERT_EQUALS(MemorySize9, MemorySize0);
        true ->
            ok
    end,

    call_gc_loop(GCSubPid, quit),
    ok = atomvm:posix_unlink(Path),
    ok.

call_gc_loop(Pid, Message) ->
    Pid ! {self(), Message},
    receive
        {Pid, Message} -> ok
    end.

gc_loop(Path, File) ->
    receive
        {select, _Resource, undefined, _Direction} ->
            gc_loop(Path, File);
        {Caller, open} ->
            {ok, Fd} = atomvm:posix_open(Path, [o_rdwr, o_creat], 8#644),
            Caller ! {self(), open},
            gc_loop(Path, Fd);
        {Caller, forget} ->
            Caller ! {self(), forget},
            gc_loop(Path, undefined);
        {Caller, gc} ->
            erlang:garbage_collect(),
            Caller ! {self(), gc},
            gc_loop(Path, File);
        {Caller, close} ->
            atomvm:posix_close(File),
            Caller ! {self(), close},
            gc_loop(Path, undefined);
        {Caller, select_write} ->
            ok = atomvm:posix_select_write(File, self(), undefined),
            Caller ! {self(), select_write},
            gc_loop(Path, File);
        {Caller, select_stop} ->
            ok = atomvm:posix_select_stop(File),
            Caller ! {self(), select_stop},
            gc_loop(Path, File);
        {Caller, quit} ->
            Caller ! {self(), quit}
    end.
