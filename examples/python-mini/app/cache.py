from functools import wraps
from typing import Any, Callable


def cached(ttl: int) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
    def decorator(fn: Callable[..., Any]) -> Callable[..., Any]:
        store: dict[Any, Any] = {}

        @wraps(fn)
        async def wrapped(*args: Any, **kwargs: Any) -> Any:
            key = (args, tuple(sorted(kwargs.items())))
            if key not in store:
                store[key] = await fn(*args, **kwargs)
            return store[key]

        return wrapped

    return decorator
