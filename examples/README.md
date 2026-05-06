# axon examples

Self-contained mini-projects you can index in seconds to see axon working.
Each example exercises a specific slice of the parser surface so you can
verify your install matches the documented behavior.

| Example                          | Lang        | Demonstrates                                          |
|----------------------------------|-------------|-------------------------------------------------------|
| [ts-mini/](ts-mini/)             | TypeScript  | decorators, async, namespaces, enums, cross-file imports |
| [python-mini/](python-mini/)     | Python      | `@router.get`, `@dataclass`, `@cached`, async, `__all__` |
| [rust-mini/](rust-mini/)         | Rust        | traits, trait impls, modules, enums, `macro_rules!`   |

## Common workflow

```bash
cd examples/<name>
axon init           # creates .axon/config.toml
axon index          # walks files + extracts symbols + embeds
axon status         # confirms file/symbol/edge counts
axon capsule "<query>"   # assembles a context capsule
```

After indexing, queries should return symbols whose `kind` matches what each
example's README claims. If a kind is wrong or missing, that's a parser
regression — file an issue with the example name and the offending output.
