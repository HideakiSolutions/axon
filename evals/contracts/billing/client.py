import httpx

ORDERS = "https://orders.example.test"

def submitOrder(order):
    return httpx.post(f"{ORDERS}/v1/orders", json=order)

def fetchOrderList():
    return httpx.get(f"{ORDERS}/v1/orders/list")

def confirmOrder(order):
    return httpx.post(f"{ORDERS}/v1/orders/confirm", json=order)

def setOrderStatus(order):
    return httpx.put(f"{ORDERS}/v1/orders/status", json=order)

def archiveOrder():
    return httpx.delete(f"{ORDERS}/v1/orders/archive")
