import { describe, expect, test } from "bun:test"
import { parseExternalCredential } from "./external-auth"

function storage(entries: Record<string, string> = {}) {
  return { getItem: (k: string) => entries[k] ?? null }
}

describe("parseExternalCredential", () => {
  test("URL params take priority", () => {
    const params = new URLSearchParams("?gb_ext_username=alice&gb_ext_credential=123.sig")
    const got = parseExternalCredential(params, storage({ "myapp.auth": '{"username":"bob","credential":"zzz"}' }), "myapp.auth")
    expect(got).toEqual({ username: "alice", credential: "123.sig" })
  })

  test("URL params trim whitespace; missing either side ignored", () => {
    expect(parseExternalCredential(new URLSearchParams("?gb_ext_username=%20alice%20&gb_ext_credential=%20sig%20"), storage(), null)).toEqual({ username: "alice", credential: "sig" })
    expect(parseExternalCredential(new URLSearchParams("?gb_ext_username=alice"), storage(), null)).toBeNull()
  })

  test("localStorage JSON object form", () => {
    const got = parseExternalCredential(new URLSearchParams(), storage({ app: '{"username":"alice","credential":"tok-1"}' }), "app")
    expect(got).toEqual({ username: "alice", credential: "tok-1" })
  })

  test("localStorage string form username:credential", () => {
    const got = parseExternalCredential(new URLSearchParams(), storage({ app: "alice:tok-1" }), "app")
    expect(got).toEqual({ username: "alice", credential: "tok-1" })
  })

  test("no storageKey and no params -> null; storage read failure -> null", () => {
    expect(parseExternalCredential(new URLSearchParams(), storage(), null)).toBeNull()
    const throwing = { getItem: () => { throw new Error("denied") } }
    expect(parseExternalCredential(new URLSearchParams(), throwing, "app")).toBeNull()
  })

  test("malformed storage values -> null", () => {
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: '{"username":42}' }), "app")).toBeNull()
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: ":only-cred" }), "app")).toBeNull()
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: "only-name:" }), "app")).toBeNull()
    expect(parseExternalCredential(new URLSearchParams(), storage({ app: "" }), "app")).toBeNull()
  })
})
