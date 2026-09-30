BROKER = "broker-a"
TOPIC = "orders.failed"
def onFailed(message): return message.value
subscribe(TOPIC, onFailed)
