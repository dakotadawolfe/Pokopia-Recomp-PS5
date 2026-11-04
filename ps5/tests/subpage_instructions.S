// SPDX-License-Identifier: GPL-3.0-or-later
// Original instruction fixtures, 64 bytes apart, for the real lifter path.
.text
.global load_acquire
.type load_acquire, %function
load_acquire:
    ldar x0, [x1]
    ret
.size load_acquire, .-load_acquire
.balign 64
.global store_release
.type store_release, %function
store_release:
    stlr x0, [x1]
    ret
.size store_release, .-store_release
.balign 64
.global exclusive_store
.type exclusive_store, %function
exclusive_store:
    ldxr x2, [x1]
    stxr w3, x0, [x1]
    ret
.size exclusive_store, .-exclusive_store
.balign 64
.global compare_swap
.type compare_swap, %function
compare_swap:
    cas x0, x2, [x1]
    ret
.size compare_swap, .-compare_swap
.balign 64
.global swap_word
.type swap_word, %function
swap_word:
    swp x0, x2, [x1]
    ret
.size swap_word, .-swap_word
.balign 64
.global store_vector
.type store_vector, %function
store_vector:
    st1 {v0.16b}, [x1]
    ret
.size store_vector, .-store_vector
.balign 64
.global zero_cache_line
.type zero_cache_line, %function
zero_cache_line:
    dc zva, x1
    ret
.size zero_cache_line, .-zero_cache_line
.balign 64
.global load_pair_exclusive
.type load_pair_exclusive, %function
load_pair_exclusive:
    ldaxp w2, w3, [x1]
    ret
.size load_pair_exclusive, .-load_pair_exclusive
