# ts-mini — TypeScript example

Three-file TypeScript project demonstrating axon's parser coverage for
decorators, async functions, namespaces, and class hierarchies.

## Try it

```bash
cd examples/ts-mini
axon init
axon index
axon status
axon capsule "user authentication"
```

## What axon should find

- `class UserService` with `@Injectable` decorator (kind="class", docstring carries decorator)
- `class AuthController` with `@Controller('/auth')` decorator
- `async function login(...)` (kind="async_function")
- `namespace Tokens` (kind="namespace")
- `enum Role` (kind="enum")
- Imports across the 3 files form an edge graph
