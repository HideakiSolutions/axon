import { Role, type User } from './types';
import { Tokens } from './tokens';

@Injectable({ providedIn: 'root' })
export class UserService {
  @Cacheable({ ttl: 60 })
  async findById(id: string): Promise<User | null> {
    const token = Tokens.signFor(id);
    return { id, role: Role.Admin, token };
  }

  async listAll(): Promise<User[]> {
    return [];
  }
}

export async function login(email: string, password: string): Promise<string> {
  const svc = new UserService();
  const u = await svc.findById(email);
  return u?.token ?? '';
}
