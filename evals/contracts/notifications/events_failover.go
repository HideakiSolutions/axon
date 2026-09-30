package notifications

const broker = "broker-a"
const topic = "orders.failed"

func consumeFailed(message []byte) {}
func startConsumer() { brokerSubscribe(broker, topic, consumeFailed) }
