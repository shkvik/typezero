// The other file of json_mix: a class that keeps a Json in a field, settings
// read into a declared type, and a parser that returns Results.
export interface Settings {
  name: string
  limit: int
  price: decimal
  tags: string[]
  note?: string
}

export class Store {
  data: Json
  constructor(text: string) {
    this.data = JSON.parse(text)
  }
  // The names of the users that have one, from whatever the file holds.
  names(): string[] {
    const users = this.data.users
    const names: string[] = []
    if (users != undefined && Array.isArray(users))
      for (const user of users.values().filter(u => u != null)) {
        const name = user.name
        if (typeof name == "string") names.push(name)
      }
    return names
  }
}

export function readSettings(text: string): Result<Settings, string> {
  const json = JSON.parse(text)
  if (json == null || typeof json != "object" || Array.isArray(json))
    return err("settings must be an object")
  return ok(fromJson<Settings>(json))
}
