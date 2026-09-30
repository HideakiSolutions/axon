public class DynamicConsumer {
    public void Call(string route) => client.GetAsync(route);
    public void Subscribe(string topic) => broker.Subscribe(topic);
}
