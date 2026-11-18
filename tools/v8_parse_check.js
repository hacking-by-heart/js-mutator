// Parses each sample with V8 (via Node's vm.Script, which compiles without
// running). Valid samples must parse; malformed samples should be rejected.
const fs = require('fs');
const vm = require('vm');
const records = fs.readFileSync(process.argv[2], 'utf8').split('\0').filter(Boolean);
let valid = 0, validOk = 0, malformed = 0, rejected = 0;
const errors = new Map();
for (const record of records) {
  const newline = record.indexOf('\n');
  const kind = record.slice(0, newline);
  const source = record.slice(newline + 1);
  let error = null;
  try { new vm.Script(source); } catch (e) { error = e.message; }
  if (kind === 'valid') {
    ++valid;
    if (!error) ++validOk;
    else errors.set(error, (errors.get(error) || 0) + 1);
  } else {
    ++malformed;
    if (error) ++rejected;
  }
}
console.log(`V8 accepted ${validOk}/${valid} valid-leaning; rejected ${rejected}/${malformed} malformed`);
for (const [message, count] of [...errors].sort((a, b) => b[1] - a[1]).slice(0, 10)) {
  console.log(`  ${count}x ${message}`);
}
// Allow a small residue: V8 itself rejects some valid code (e.g. f([7], [a] = 1)).
process.exit(validOk * 1000 >= valid * 995 && rejected * 100 >= malformed * 95 ? 0 : 1);
