-module(test_atomvm_random).
-export([start/0]).

start() ->
    RandomTuple = random_tuple(),
    1 = all_integer(RandomTuple),
    Test1 = all_equal(RandomTuple),

    Bin = atomvm:rand_bytes(4),
    RandomTuple2 = binary_to_tuple(Bin),
    Test2 = all_equal(RandomTuple2),
    Test1 + Test2 * 2.

all_integer({A, B, C, D}) when is_integer(A) and is_integer(B) and is_integer(C) and is_integer(D) ->
    1;
all_integer(_) ->
    0.

all_equal({A, B, C, D}) when (A =/= B) and (B =/= C) and (C =/= D) ->
    0;
all_equal(_) ->
    1.

random_tuple() ->
    {atomvm:random(), atomvm:random(), atomvm:random(), atomvm:random()}.

binary_to_tuple(B) ->
    L = erlang:binary_to_list(B),
    erlang:list_to_tuple(L).
