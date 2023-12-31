%
% This file is part of AtomVM.
%
% Copyright 2019 Fred Dushin <fred@dushin.net>
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

-module(test_min_heap_size).

-export([start/0]).

start() ->
    ok = test_min_heap_size(1000),
    ok = test_min_heap_size(5000),
    0.

test_min_heap_size(MinSize) ->
    {Pid1, Ref1} = spawn_opt(
        fun() ->
            % Heap size is set to minimum at first GC/Heap growth
            alloc_some_heap_words(),
            {total_heap_size, TotalHeapSize} = process_info(self(), total_heap_size),
            case erlang:system_info(machine) of
                "BEAM" ->
                    true = TotalHeapSize >= MinSize;
                _ ->
                    TotalHeapSize = MinSize
            end
        end,
        [monitor, {min_heap_size, MinSize}]
    ),
    ok =
        receive
            {'DOWN', Ref1, process, Pid1, normal} -> ok
        after 500 -> timeout
        end,
    ok.

alloc_some_heap_words() ->
    alloc_some_heap_words(20, []).

alloc_some_heap_words(0, _Acc) -> ok;
alloc_some_heap_words(N, Acc) -> alloc_some_heap_words(N - 1, [N | Acc]).
