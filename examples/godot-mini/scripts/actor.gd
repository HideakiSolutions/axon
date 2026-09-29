class_name Actor
extends Node2D

signal health_changed(value: int)

enum State { IDLE, HURT }

@export var max_health: int = 3

var health: int = max_health
var state: State = State.IDLE

func take_damage(amount: int) -> void:
	health = maxi(0, health - amount)
	state = State.HURT
	health_changed.emit(health)
