export async function publishFailed(producer, order) {
  await producer.send({topic: 'orders.failed', messages: [{value: JSON.stringify(order)}]});
}
export const broker = 'broker-a';
