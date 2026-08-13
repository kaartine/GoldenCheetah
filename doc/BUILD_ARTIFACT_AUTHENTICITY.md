# Build artifact authenticity

GoldenCheetah's authenticated CI artifact transport is the current authenticity boundary for build outputs. Consumers must retrieve CI artifacts while
authenticated to the expected GitHub Actions or AppVeyor project and verify the
attached provenance, manifest, and SBOM before promotion.

This boundary does not provide independently verifiable artifact signatures.
Adding release signing requires separate key ownership, protected signing jobs,
rotation and revocation procedures, and client-side verification policy. That is
a release-engineering feature outside BUILD-001's reproducibility and provenance
scope. Public macOS snapshot publication runs only from a trusted push to
`master`; pull-request candidates never receive release credentials or a
write-capable repository token.

## Repository rules required outside Git

The checked-in ruleset is the feasible single-maintainer baseline: it requires
pull requests and status checks, blocks deletion and force pushes, and has no
bypass actors. It intentionally requires zero approving reviews and disables
Code Owner and last-push approval because GitHub does not allow the sole author
to approve their own pull request. Enabling those settings now would make every
change impossible to merge.

When a second maintainer is available, require at least one approval, Code Owner
review, and last-push approval. The checked-in `CODEOWNERS` file already covers
trusted workflow policy and release packaging for that stronger configuration.

Configure these exact commit-status contexts as required before merge:

- `Workflow policy / immutable actions`
- `Candidate CI / build`
- `Candidate CI / native activity transactions`
- `Candidate CI / durable and anchored filesystem`

The workflow-policy status checks candidate workflows as untrusted data using
the checker and semantic contract from the protected base revision. Each
downstream workflow publishes pending and final status directly to the
candidate commit. Its status reporters are isolated jobs; jobs that check out
candidate code have no `statuses: write` permission.

The protected workflow set is closed, and each workflow's semantics are
hash-bound by the contract from the base revision. An intentional workflow
change therefore uses a reviewed rotation: first add both the current and
proposed semantic hashes to the contract, then merge the workflow change, and
finally remove the retired hash. Contract and workflow changes in the same
pull request do not authorize themselves.
