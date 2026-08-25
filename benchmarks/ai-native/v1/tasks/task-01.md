# Task 1

File: `benchmark.kai`

`calculate_shipping` needs to support optional shipping insurance. Change
`calculate_shipping` so that it takes an additional `insurance_flag: bool`
parameter. When `insurance_flag` is `true`, add a flat surcharge of `150` to
the shipping cost it would otherwise return; when `false`, behavior is
unchanged.

Update every place in the file that calls `calculate_shipping` so the program
keeps compiling and behaving correctly:

- Inside `process_order`, insurance should be included automatically for
  members (i.e. pass the order's existing membership status as the insurance
  flag).
- Everywhere else that calls `calculate_shipping`, insurance should stay
  disabled (pass `false`).

Do not change any other function's behavior.

## Completion criteria

- `benchmark.kai` compiles successfully with `kaicc`.
- The resulting executable exits successfully.
- Running the executable produces exactly the expected stdout for this task.
