# WASM candidate lifecycle

This private implementation prepares cooperative Emscripten side modules.
It is not yet connected to `reload_session` or exposed by a backend factory.
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
of the candidate and loader object's lifetime. Completion can be synchronous
when Emscripten already has that immutable path loaded.

Uncommitted images call `dlclose` when discarded. This releases the adapter's
reference, not a promise that Emscripten reclaims code, linear memory or table
slots. There is no committed-module unloading policy yet.

Modules must have side-effect-free initialization and descriptor access.
Validation happens after instantiation; it cannot roll back arbitrary module
constructors, traps or writes into shared memory. Descriptor pointers are
trusted under the cooperative ABI, not treated as hostile serialized input.
Remote publication, digest checks and pre-instantiation metadata belong to the
subsequent generation-input work. Paths must refer to immutable contents.
