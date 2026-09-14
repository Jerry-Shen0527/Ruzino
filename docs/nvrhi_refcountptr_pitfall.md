# nvrhi `RefCountPtr`: never take `&handle`

nvrhi's `RefCountPtr<T>` (`external/nvrhi/include/nvrhi/common/resource.h:307`)
overloads unary `operator&()` to return `T**` — a pointer to the inner `ptr_`
member — **not** `RefCountPtr*`. Code that "takes the handle's address"
therefore compiles against a plausible-but-wrong type and corrupts state
silently. Two confirmed instances in this codebase:

## Case 1 — renderer readback (`renderDelegate.cpp`)

`return VtValue(&it->second)` where `it->second` was a `TextureHandle`: the `&`
returned `ITexture**` pointing at the `ptr_` member; the caller read a
different address and got NULL. Fix: return the pointer directly
(`VtValue(reinterpret_cast<const void*>(tex_ptr))`).

## Case 2 — Wetbrush fluid solve (`node_brush_wb_fluid.cpp`)

The velocity diffuse/advect loops used `std::make_pair(&field->vel_x,
&field->vel_x_tmp)` then `std::swap(*pair.first, *pair.second)`. Because
`&field->vel_x` yields `IBuffer**` (the `ptr_` member address, not the
`RefCountPtr*`), the swap exchanged raw `IBuffer*` values, bypassing refcount
accounting and nulling `field->vel_x/y/z` mid-solve — the next `copyBuffer`
crashed in `nvrhi::requireBufferState` (`mov rax,[rax]`, `rax=0`). Fix: take
the true object address with `std::addressof(...)`.

## The rule

Never write `&someRefCountPtr`. If you need a `RefCountPtr*` (e.g. to swap two
handles), use `std::addressof(handle)` — it bypasses the overloaded
`operator&()` and returns the real object address, so `*addr` is a correct
`RefCountPtr&` alias and `std::swap` goes through the move operators (proper
AddRef/Release).

Grep the codebase for `make_pair(&` and `&field->`/`&it->` patterns whenever a
`RefCountPtr` (any `nvrhi::*Handle`) is involved.

## Why this class of bug is nasty

`operator&()` returns a valid pointer of a *plausible* type, so the code
compiles and the corruption shows up only as a later NULL-deref crash far from
the cause — exactly the "bare access violation, no useful stack" scenario the
cdb workflow in the root `AGENTS.md` (Troubleshooting → Native Debugging with
cdb) was written for.
