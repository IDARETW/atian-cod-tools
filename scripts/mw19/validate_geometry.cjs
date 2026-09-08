// Optional independent Khronos validator. Install only into the ignored build tree:
// npm install --prefix build/mw19-tests/gltf-validator --ignore-scripts gltf-validator@2.0.0-dev.3.10
const path = require('path');
const fs = require('fs');
const {execFileSync} = require('child_process');
const root = path.resolve(__dirname, '../..');
const validator = require(path.join(root, 'build/mw19-tests/gltf-validator/node_modules/gltf-validator'));
const bytes = execFileSync(path.join(root, 'build/mw19-tests/mw19-mesh-test.exe'), ['--emit-glb']);
validator.validateBytes(new Uint8Array(bytes), {uri: 'fixture.geometry.glb', maxIssues: 100}).then(report => {
    if (process.argv[2]) fs.writeFileSync(process.argv[2], JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify({validator: validator.version(), bytes: bytes.length, issues: report.issues}));
    if (report.issues.numErrors || report.issues.numWarnings) process.exitCode = 1;
}).catch(error => { console.error(error); process.exitCode = 1; });
