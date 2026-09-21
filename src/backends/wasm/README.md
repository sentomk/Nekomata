# WASM candidate lifecycle

This private implementation prepares cooperative Emscripten side modules.
It has a private browser session, but is not yet connected to the public
`neko::reload_session` or exposed by a backend factory.
Application state never enters the loader: it remains owned by the main module.

`candidate` starts an asynchronous load and validates the descriptor's version,
size, ABI identity and exact ordered entry membership. Entry names and function
addresses are copied into an owned, immutable prepared module. An ABI identity
must cover the application-defined signatures and state layout; validation
compares that declared identity, not DWARF or actual function signatures.

`active_module::activate()` consumes only a ready candidate, pins its code to
page lifetime and swaps the complete prepared module without allocating or
calling application behavior. The application must establish a safe point.
`current()` returns an owning snapshot, so a caller can use one consistent
entry set for a whole frame. Prior committed code stays resident even after
the active module and snapshots are destroyed.

All operations and completions run on one event loop. Destroying or cancelling
a candidate discards pending results; it does not abort a browser download.
The loader owns its request path and callback until completion, independently
of the candidate and loader object's lifetime. The loader interface permits
synchronous completion; the browser fetch adapter completes asynchronously.

The private `reload_session` registers one group, initially disabled.
`watch()` or `watch(group_id)` starts scheduled polling; `unwatch()` or
`unwatch(group_id)` stops new polling and consumption without discarding the
cursor, pending candidate or active code. In-flight requests may finish into
retained state. `update()` only consumes prepared results while enabled.
Repeated enable/disable calls are idempotent; an unknown group is a
configuration error. Old scheduling callbacks cannot revive after re-enable.

`poll_scheduler` separates observation from frame commits. Its owning
subscription stops scheduling when destroyed; the browser implementation
uses a 100 ms event-loop interval. A session's loader, fetcher and scheduler
must outlive it. Native tests advance scheduled ticks explicitly, without
network or sleeps. The browser scheduler has its own real-event-loop test.

Uncommitted images call `dlclose` when discarded. This releases the adapter's
reference, not a promise that Emscripten reclaims code, linear memory or table
slots. There is no committed-module unloading policy yet.

Modules must have side-effect-free initialization and descriptor access.
Validation happens after instantiation; it cannot roll back arbitrary module
constructors, traps or writes into shared memory. Descriptor pointers are
trusted under the cooperative ABI, not treated as hostile serialized input.
The HTTP adapter checks artifact SHA-256 before instantiation; this does not
authenticate the publisher or validate the descriptor before initialization.
Paths must refer to immutable contents.
