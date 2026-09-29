extends Node2D

const ActorScript = preload("res://scripts/actor.gd")

func _ready() -> void:
	var actor = ActorScript.new()
	add_child(actor)
	actor.health_changed.connect(_on_health_changed)
	actor.take_damage(1)

func _on_health_changed(value: int) -> void:
	print("Health: ", value)
