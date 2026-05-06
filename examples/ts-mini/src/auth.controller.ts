import { UserService, login } from './user.service';

@Controller('/auth')
export class AuthController {
  constructor(private users: UserService) {}

  @Post('/login')
  async loginRoute(email: string, password: string) {
    return { token: await login(email, password) };
  }
}
