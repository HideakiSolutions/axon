from dataclasses import dataclass

__all__ = ["User"]


@dataclass
class User:
    id: str
    name: str

    @property
    def display_name(self) -> str:
        return f"{self.name} ({self.id})"
