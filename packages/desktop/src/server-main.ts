import { startServer } from "@gebai/server"

if (import.meta.main) {
  startServer().catch((err) => {
    console.error(err)
    process.exit(1)
  })
}
