BROKER = "broker-a"
TOPIC = "orders.cancelled"
def onCancelled(message): return message.value
subscribe(TOPIC, onCancelled)
