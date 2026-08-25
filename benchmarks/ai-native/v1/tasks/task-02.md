# Task 2

File: `benchmark.kai`

There is a pricing bug in `calculate_discount`: for large-volume orders
(`quantity >= 10`), the volume discount is currently allowed to reduce the
order all the way down to `0` if the raw volume discount happens to be large
enough. Volume discounts should never exceed 40% of the subtotal, no matter
how large the raw computed volume discount is.

Fix `calculate_discount` so that only the volume-discount branch (the
`quantity >= 10` case) enforces this new 40% cap. The member-discount
behavior (orders that don't hit the volume-discount branch, or the
member-discount component within it) must remain exactly as it is today.

## Completion criteria

- `benchmark.kai` compiles successfully with `kaicc`.
- The resulting executable exits successfully.
- Running the executable produces exactly the expected stdout for this task.
