import { describe, expect, test } from "bun:test"
import { injectPlantUmlLayout } from "./plantuml-layout"

describe("injectPlantUmlLayout", () => {
  test("injects spacing defaults for @startuml when not explicitly set", () => {
    expect(injectPlantUmlLayout("@startuml\nAlice -> Bob\n@enduml")).toBe(
      "@startuml\nAlice -> Bob\nskinparam ranksep 80\nskinparam nodesep 40\n@enduml",
    )
  })

  test("respects explicit ranksep/nodesep skinparams", () => {
    expect(injectPlantUmlLayout("@startuml\nskinparam nodesep 10\nA -> B\n@enduml")).toBe(
      "@startuml\nskinparam nodesep 10\nA -> B\n@enduml",
    )
    expect(injectPlantUmlLayout("@startuml\nskinparam ranksep 120\nA -> B\n@enduml")).toBe(
      "@startuml\nskinparam ranksep 120\nA -> B\n@enduml",
    )
  })

  test("appends @enduml when missing", () => {
    expect(injectPlantUmlLayout("@startuml\nA -> B")).toBe(
      "@startuml\nA -> B\nskinparam ranksep 80\nskinparam nodesep 40\n@enduml",
    )
  })

  test("leaves non-uml diagram types untouched (structure-driven layout)", () => {
    expect(injectPlantUmlLayout("@startmindmap\n* 根\n@endmindmap")).toBe("@startmindmap\n* 根\n@endmindmap")
    expect(injectPlantUmlLayout("@startwbs\n* 项目\n@endwbs")).toBe("@startwbs\n* 项目\n@endwbs")
    expect(injectPlantUmlLayout("@startgantt\n[任务] lasts 3 days\n@endgantt")).toBe(
      "@startgantt\n[任务] lasts 3 days\n@endgantt",
    )
  })
})
