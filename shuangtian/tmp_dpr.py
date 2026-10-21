import json, urllib.request, subprocess, time, pathlib
VSCODE=r"C:\Users\Administrator\AppData\Local\Programs\Microsoft VS Code\Code.exe"
ROOT=pathlib.Path(r"C:\Users\Administrator\code\gebai\shuangtian")
sample=ROOT/"build/dwab/vscode-sample.py"
proc=subprocess.Popen([VSCODE,str(sample),"--new-window","--disable-extensions",
  f"--user-data-dir={ROOT/'.st/vscode-cdp-profile'}","--remote-debugging-port=9333","--locale","en"])
time.sleep(12)
try:
    with urllib.request.urlopen("http://127.0.0.1:9333/json/list",timeout=5) as r:
        pages=[t for t in json.loads(r.read()) if t.get("type")=="page"]
    from websockets.sync.client import connect
    ws=connect(pages[0]["webSocketDebuggerUrl"])
    ws.send(json.dumps({"id":1,"method":"Runtime.evaluate","params":{"expression":
      "JSON.stringify({dpr:devicePixelRatio,w:innerWidth,h:innerHeight,"
      "fs:getComputedStyle(document.querySelector('.monaco-editor')||document.body).fontSize})","returnByValue":True}}))
    while True:
        m=json.loads(ws.recv())
        if m.get("id")==1:
            print("RESULT:",m["result"]["result"].get("value")); break
    ws.close()
finally:
    proc.terminate(); subprocess.run(["taskkill","/F","/IM","Code.exe"],capture_output=True)
