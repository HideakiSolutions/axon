export enum Role {
  Admin = 'admin',
  Member = 'member',
  Guest = 'guest',
}

export interface User {
  id: string;
  role: Role;
  token: string;
}

export type UserId = string;
