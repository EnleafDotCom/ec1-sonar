import base64,sys
s=open(sys.argv[1]).read().replace('/*WASM*/',base64.b64encode(open(sys.argv[2],'rb').read()).decode())
open(sys.argv[3],'w').write(s); print(len(s))
