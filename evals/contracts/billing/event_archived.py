BROKER = "broker-a"
TOPIC = "orders.archived"
def onArchived(message): return message.value
subscribe(TOPIC, onArchived)
