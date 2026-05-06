from fastapi import APIRouter

from .models import User
from .cache import cached

router = APIRouter()


@router.get("/users")
async def list_users() -> list[User]:
    return await _load_users()


@router.get("/users/{user_id}")
async def get_user(user_id: str) -> User | None:
    return User(id=user_id, name="alice")


@cached(ttl=60)
async def _load_users() -> list[User]:
    return [User(id="1", name="alice"), User(id="2", name="bob")]
