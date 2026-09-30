export async function publishCreatedReplica(producer, order) {
  await producer.send({topic: 'orders.created', messages: [{value: JSON.stringify(order)}]});
}
export const broker = 'broker-b';
