# python-mini — FastAPI example

Three-module Python project demonstrating axon's parser coverage for
decorated routes, async handlers, dataclasses, and `__all__` exports.

## Try it

```bash
cd examples/python-mini
axon init
axon index
axon capsule "user routes"
```

## What axon should find

- `async def list_users(...)` decorated with `@router.get("/users")` (kind="async_function", docstring carries decorator)
- `@dataclass class User` (kind="class", docstring shows `@dataclass`)
- `@cached(ttl=60)` decorated synchronous helpers
- Cross-module imports between routes/models/cache
