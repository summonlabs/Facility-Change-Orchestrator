# Contributing to Facility Change Orchestrator

Thanks for taking the time to contribute. This document describes how to build,
test, and submit changes to the project.

## Contribution licensing

This project is licensed under the Apache License, Version 2.0 (see [LICENSE](LICENSE)).
Inbound contributions are accepted under the same terms: unless you explicitly state
otherwise, any contribution you intentionally submit for inclusion in the work is
licensed under the Apache License, Version 2.0, per section 5 of that license, with no
additional terms or conditions. There is **no Contributor License Agreement (CLA)** to
sign and **no copyright assignment**: you keep the copyright on your contributions.

By opening a pull request you confirm that you are the author of the change, or that you
have the right to submit it under these terms.

## Building and testing

Requirements:

- CMake 3.21 or newer
- A C++20 compiler: MSVC (Visual Studio 2022 or newer), GCC 11+, or Clang 13+
- A generator and build tool appropriate for your platform (Visual Studio, Ninja, or Make)

Configure, build, and test from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The `--config Release` arguments matter for multi-configuration generators such as
Visual Studio; single-configuration generators use `CMAKE_BUILD_TYPE`. Run the full test
suite before opening a pull request, and make sure it passes on every platform you can
reasonably reach.

## Code quality expectations

- **C++20.** Use the standard library first. Reach for a third-party library only when the
  standard library genuinely cannot express the requirement.
- **Zero first-party warnings.** The project builds with `/W4 /WX` on MSVC and
  `-Wall -Wextra -Werror` elsewhere. A change that introduces a warning does not build,
  so fix the cause rather than suppressing the diagnostic.
- **No new dependencies without a compelling systems reason.** A dependency is a permanent
  maintenance, licensing, and supply-chain obligation. Explain the reason in the pull
  request, or implement the small piece you need.

## Design expectations

- **Determinism.** The same inputs must produce the same outputs and the same ordering.
  Do not let iteration order, wall-clock time, thread scheduling, or pointer values leak
  into observable behavior.
- **Explicit ownership.** State clearly who owns what and for how long. Avoid implicit
  sharing and hidden lifetime coupling; lifetimes belong in the type system, not in
  comments.
- **Explicit generations, epochs, and authority.** State that changes hands must carry the
  generation, epoch, or authority that produced them, and reject stale or unauthorized
  inputs explicitly rather than resolving them heuristically.
- **Precise error semantics.** Every failure mode should be distinguishable, documented,
  and actionable. Do not collapse unrelated failures into a single generic error, and do
  not use exceptions or status codes interchangeably within one interface.

## Tests are proof obligations

Tests are not decoration; they are the evidence that a change does what it claims.

- New behavior requires tests that would fail without the change.
- A bug fix requires a regression test that reproduces the original defect.
- Keep tests deterministic and independent of execution order, wall-clock time, and host
  configuration; a flaky test is a defect in the test.

## Commit messages

Write concise, neutral, imperative commit messages ("Add digest comparison", not "Added"
or "Adds"). Describe the change itself, not the process that produced it.

Do not add AI attribution, "generated with" lines, or `Co-authored-by` trailers. Keep the
author list to the humans who wrote and reviewed the change.

## Reporting bugs

Open an issue and include:

- what you expected to happen and what actually happened;
- a minimal reproduction, ideally a short program or test case;
- the exact commands you ran, plus compiler, version, platform, and CMake version;
- any diagnostics, assertion text, or logs, pasted verbatim.

Search existing issues first, and keep one issue per distinct problem.

## Proposing changes

- Open an issue before large or design-level changes so the approach can be discussed
  before you invest in the implementation.
- Keep pull requests small and focused; unrelated cleanups belong in a separate change.
- Reference the issue the pull request addresses, and describe the behavior change, the
  reasoning behind it, and any tradeoffs.
- Update tests and documentation in the same change as the code.

## Telemetry

The project does not transmit telemetry. It does not collect or send usage data, and
contributions must not add telemetry, analytics, or phone-home behavior.
