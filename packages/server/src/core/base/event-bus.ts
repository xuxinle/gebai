import type { AgentEvent } from "@gebai/sdk"

export class EventBus {
  private subscribers = new Set<(e: AgentEvent) => void>()

  subscribe(fn: (e: AgentEvent) => void): () => void {
    this.subscribers.add(fn)
    return () => this.subscribers.delete(fn)
  }

  publish(event: AgentEvent): void {
    for (const fn of this.subscribers) {
      try {
        fn(event)
      } catch {
        /* subscriber errors must not break the loop */
      }
    }
  }

  get subscriberCount(): number {
    return this.subscribers.size
  }
}
