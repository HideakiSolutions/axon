export async function publishArchived(producer, order) {
  await producer.send({topic: 'orders.archived', messages: [{value: JSON.stringify(order)}]});
}
export const broker = 'broker-a';
