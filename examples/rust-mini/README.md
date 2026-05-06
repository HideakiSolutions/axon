# rust-mini — Rust example

Cargo crate demonstrating axon's parser coverage for traits, trait impls,
modules, enums, and declarative macros.

## Try it

```bash
cd examples/rust-mini
axon init
axon index
axon capsule "Greeter trait"
```

## What axon should find

- `trait Greeter` (kind="trait")
- `impl Greeter for EnUS` and `impl Greeter for PtBR` (kind="impl", names "Greeter for EnUS" / "Greeter for PtBR")
- `enum Locale` (kind="enum")
- `mod inner` (kind="module")
- `macro_rules! shout` (kind="macro")
- `struct EnUS`, `struct PtBR` (kind="class" — back-compat)
