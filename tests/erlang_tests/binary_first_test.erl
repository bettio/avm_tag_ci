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

-module(binary_first_test).

-export([start/0, id/1, firstp10/1]).

start() ->
    firstp10(id(<<"HelloWorld">>)) + firstp10safe(<<>>) + firstp10safe(42) + firstp10safe({<<>>}).

firstp10(Bin) ->
    binary:first(Bin) + 10.

firstp10safe(Bin) ->
    try firstp10(Bin) of
        _Any -> 1
    catch
        error:badarg -> 0;
        _:_ -> -1
    end.

id(X) ->
    X.
