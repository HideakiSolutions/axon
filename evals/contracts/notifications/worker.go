package notifications

import "net/http"

func archiveOrder() (*http.Response, error) {
    return http.Get("https://orders.example.test/v1/archive")
}
func fetchOrders() (*http.Response, error) {
    return http.Get("https://orders.example.test/v1/orders/list")
}
func notify() { /* unrelated RPC */ }
