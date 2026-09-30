export async function publishConfirmed(producer, order) {
  await producer.send({topic: 'orders.confirmed', messages: [{value: JSON.stringify(order)}]});
}
export const broker = 'broker-a';
