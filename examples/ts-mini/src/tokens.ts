export namespace Tokens {
  export function signFor(userId: string): string {
    return `tok_${userId}_${Date.now()}`;
  }

  export function verify(token: string): boolean {
    return token.startsWith('tok_');
  }
}
