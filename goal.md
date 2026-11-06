# Native Pokopia PS5: autonomous development goal

## Objective

Deliver reliable native Pokopia gameplay on PS5 through sustained research,
implementation and automated console testing. Substantially reduce startup time,
resolve black-screen, rendering and synchronization failures, establish genuine
interactive gameplay, then improve stability and measured performance toward
60 real FPS with frame generation disabled.

Once gameplay is established, continue through the product requirements in
`ps5/RELEASE_REQUIREMENTS.md`. A private development executable is an intermediate
milestone, not completion of the native-port goal.

This document defines work for the agent assigned the goal. Writing or reading it
alone does not start console operations.

## Establish current state

Work from this repository, even if the session starts in the sibling Pokopia
checkout. Read repository instructions, `ps5/README.md`,
`ps5/RELEASE_REQUIREMENTS.md`, and the relevant implementation documents.

On the existing development machine, also read the private handoffs
`work/ps5/AUTOMATED_TESTING.md` and `work/ps5/NATIVE_PORT_PLAN.md`, plus the newest
installation receipts, game reports and kernel captures. These ignored files
contain environment-specific commands and evidence; never force-add them.
If they are unavailable, use tracked tooling and documentation to establish the
environment without inventing endpoints, paths, hashes or previous results.

Check Git status, branch and remotes; distinguish existing work from your changes.
Identify the installed build, current title state, console services, owned helpers
and any running experiment. Preserve available evidence before changing state.
Treat historical PIDs, hashes, service availability and diagnoses as potentially
stale. Resolve conflicting notes using dated receipts and direct observations.

## Authorization and user involvement

The owner authorizes sustained reversible development and automated testing on
the existing owned PS5 setup: local builds, focused diagnostics, verified
replacement of this project's test application, native launches, one-use selection
of the previously saved validated game folder, bounded read-only debugger
sampling, frame capture, log collection and iterative fixes.

This authorization supersedes historical manual-launch-only restrictions for this
goal. Do not ask permission between ordinary research, edits, builds, deployments,
test runs or supported recovery of your own temporary tooling.

Ask the owner only when progress requires actual controller input, physical
screen/audio confirmation, or physical console recovery that available tools
cannot perform. Request one precise action or observation, explain why automation
cannot establish it, and continue useful independent work while waiting.

Do not expand this authorization to destructive storage operations, firmware
modification, unrelated applications, external registrations or public distribution
of private artifacts. Choose an authorized alternative when an experiment would
require such actions. Never destroy existing saves or original game files.

## Automated execution

Validate the newly created workflow end to end before relying on it. A prepared
script is not evidence that its first native launch, debugger attach or frame
capture has succeeded. Use the documented console-probe, executable-replacement,
native-launch, autorun, sampling and collection mechanisms. Fix or extend them
when necessary, with focused checks and explicit resource ownership.

Before replacement, verify the intended title is stopped and unmounted. Preserve
the previous executable, exact symbols, build manifest and rollback identity.
Verify complete installed-file hashes by readback. Validate only the remembered
game folder for unattended selection; do not silently substitute another input.

After launch, require the actual native process, a fresh matching report,
successful autorun validation and advancing startup evidence. A launcher return
code, uploaded executable or accepted payload does not prove native execution.

Run one console experiment at a time. Use bounded monitoring and stop conditions.
Collect application reports, kernel faults, captures and timing evidence before
relaunching or restarting. Never kill unrelated applications to make room.

Keep debugger operations within the documented read-only protocol and verified
target identity. Refuse interference with an existing debug session. Track your
attachment, resume and detach; verify continuing application progress. Connection
loss does not prove successful detachment. Do not write guest state, force queue
flags or bypass synchronization to manufacture apparent progress.

## Research and iteration loop

1. Identify the highest-value unresolved failure or measured bottleneck.
2. Record a concrete hypothesis, supporting evidence, competing explanations and
   an experiment that can distinguish them. Define success and failure signals.
3. Research primary sources and exact upstream revisions when needed. Check
   applicability, dependencies, previous attempts and whether a fix is already
   present. Another platform's result is a lead, not a PS5 measurement.
4. Implement a coherent batch of related changes. Separate unrelated speculative
   fixes so regressions remain attributable and rollback is practical.
5. Run focused host checks and appropriate native component tests. Exercise real
   behavior and failure paths rather than tests that merely mirror implementation.
6. Build, retain exact symbols and provenance, verify installation, launch and
   collect the bounded native experiment automatically.
7. Compare with the baseline. Accept, revise or revert based on evidence; record
   what remains unknown and choose the next discriminating action.
8. Update durable research notes and continue without requesting routine approval.

Take research breaks whenever results are ambiguous or the next build lacks a
defensible hypothesis. Use exact-symbol analysis, bounded thread samples, private
captured replay and small reproducers to reduce repeated full-startup runs.
Repeated failure without new evidence calls for a different experiment, not the
same blind retry. Extend a watchdog only when observed progress justifies more
time; a longer timeout is not a startup-speed fix.

## Priorities and measurement

First establish dependable unattended launch, collection, sampling and capture.
Then explain expensive initialization and later waits, inspect actual output and
resource state, and fix demonstrated execution or rendering defects. Reach a
recognizable menu, responsive controls and world progression before pursuing
cosmetic work or broad optimization.

Measure CPU execution, synchronization, file I/O, texture processing, shader
lookup and GPU work separately. Do not attribute the whole startup delay to
shader compilation without measurements. Thread snapshots are not CPU usage;
stale counters do not prove idle workers; overlapping timings are not additive.

Use a consistent scene, settings and measurement window. Separate cold and warm
startup, real guest FPS and generated output FPS, and diagnostic overhead from
ordinary operation. Reduce instrumentation after it answers its question.

Draws, submissions, presentations, nonblack pixel counts and successful shader
validation do not independently prove recognizable or correct gameplay. Inspect
captured frames, then request the minimum necessary physical confirmation for
screen behavior, control response or audio that automation cannot establish.

## Evidence and acceptance

Maintain a resumable ledger with source revision and relevant dirty-patch identity,
hypothesis, build hashes, installation verification, test conditions, timings,
observations, faults, rollback location and next action. Keep private evidence
private; publish only appropriate redacted findings and reproducible source.
Preserve contradictory results and rejected hypotheses.

Clearly distinguish source analysis, host tests, payload tests, native-app
execution, captured pixels and physical gameplay confirmation. One non-faulting
run does not isolate a fix or establish reliability.

Gameplay acceptance requires repeatable native launches, recognizable correct
imagery, responsive controls, loading and progressing through a world, functioning
audio, save/load and clean exit without unexplained faults. Specify representative
test conditions and report actual results. Physical checks remain pending until
observed; do not substitute counters or assumptions.

Treat 60 FPS as a target until measured. If evidence establishes a practical
limitation, document it and pursue supported improvements without inventing
success or silently redefining acceptance.

After gameplay, implement and verify the release requirements: on-console
preparation from user-provided decrypted inputs, title-owned installation,
interruption recovery, native PS5 save management and uninstall cleanup. Preserve
saves and originals throughout. PC-generated private modules and shader caches
do not satisfy release acceptance. Validate a generic package without developer
outputs before calling the product ready; do not publish game-derived material.

Do not declare completion at a build, deployment, diagnostic pass, first frame or
partial milestone. Continue until required acceptance is met. If a necessary
physical intervention is pending, state the exact blocker and remaining checks
while pursuing any independent work that can still advance the goal.

## Repository hygiene and communication

Preserve unrelated dirty changes and other agents' work. Inspect overlapping
changes before editing; do not discard, claim or commit work you did not make.
Build locally, not through GitHub builds. Keep game-derived code, executables,
shaders, caches, captures, dumps and private logs outside public Git and releases.

Commit only your public source, tests and documentation as `dakotadawolfe`, using
ordinary human commit messages. Run source-artifact checks before committing.
Integrate completed changes into main and push only to Dakota's own origin.
Public tooling must be reproducible from tracked source and documented
dependencies; private game inputs remain explicitly external. Remove only your
completed temporary branches/worktrees after verifying their work is integrated.

Close owned programs, collectors, build servers and helper sessions when no
longer needed and at task end. Do not terminate pre-existing user services.
Verify cleanup targets and remove only disposable owned scratch data, following
the owner's deletion rules. Preserve useful evidence and rollback artifacts.

Give concise updates on findings, decisions, meaningful milestones and necessary
physical actions. Keep a clear next step across interruptions and resumptions.
Continue autonomously through research, implementation and testing rather than
ending with an offer to continue.
