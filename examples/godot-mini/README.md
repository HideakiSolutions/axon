# godot-mini — GDScript example

A minimal Godot 4 project with a scene and two scripts. Axon indexes `.gd`
files, extracts classes, functions, signals and enums, and records the
`preload("res://scripts/actor.gd")` dependency.

```bash
cd examples/godot-mini
axon init
axon index
axon status
axon capsule "actor health changed"
```

Open `project.godot` in Godot 4 to run the scene. It creates an `Actor`,
applies one point of damage and prints the updated health.

Axon parses GDScript source files. Scene (`.tscn`) and resource (`.tres`)
files are included as a runnable Godot example, but their contents are not
indexed as symbols.
