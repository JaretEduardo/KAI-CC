# Task 3

File: `benchmark.kai`

We want every place that computes a risk score from `risk_score_for_order`
to consistently apply the risk penalty (`compute_risk_penalty`) to that raw
score before using it further. One place in the file already does this
correctly today; another place computes a risk score but does not apply the
penalty at all. Find every direct call site of `risk_score_for_order` and
make sure each one applies the penalty the same way (raw score minus the
penalty) before returning or otherwise using the resulting value.

Do not change `risk_score_for_order` or `compute_risk_penalty` themselves,
and do not change the call site that already applies the penalty correctly.

## Completion criteria

- `benchmark.kai` compiles successfully with `kaicc`.
- The resulting executable exits successfully.
- Running the executable produces exactly the expected stdout for this task.
