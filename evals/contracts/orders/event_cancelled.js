export async function publishCancelled(producer, order) {
  await producer.send({topic: 'orders.cancelled', messages: [{value: JSON.stringify(order)}]});
}
export const broker = 'broker-a';
