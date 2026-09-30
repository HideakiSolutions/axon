package notifications

const broker = "broker-b"
const topic = "orders.created"

func consumeCreated(message []byte) {}

func startConsumer() { brokerSubscribe(broker, topic, consumeCreated) }
