%
% This file is part of AtomVM.
%
% Copyright 2019 Davide Bettio <davide@uninstall.it>
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

-module(improper_cmp).

-export([start/0, id/1, add/2]).

start() ->
    L1 = id([id(add(id(1), id(0))), id(add(id(1), id(1))) | id(add(id(1), id(2)))]),
    L2 = id([id(add(id(1), id(0))), id(add(id(1), id(1))) | id(add(id(1), id(3)))]),
    L3 = id([id(add(id(1), id(0))), id(add(id(1), id(1))) | id(<<"test">>)]),
    cmp(L1, L2) + cmp(L1, L3) * 2.

id(I) ->
    I.

add(A, B) ->
    id(A) + id(B).

cmp(L1, L2) when is_list(L1) and is_list(L2) ->
    if
        L1 == L2 -> 0;
        L1 > L2 -> -1;
        L1 < L2 -> 1
    end.
