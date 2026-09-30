BROKER = "broker-a"
TOPIC = "orders.created"

def onCreated(message):
    return message.value

subscribe(TOPIC, onCreated)
