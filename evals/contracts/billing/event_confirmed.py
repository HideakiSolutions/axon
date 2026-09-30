BROKER = "broker-a"
TOPIC = "orders.confirmed"
def onConfirmed(message): return message.value
subscribe(TOPIC, onConfirmed)
