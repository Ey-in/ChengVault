"""Check model field/ref types using the official StarUML metamodel project.

The upstream MDJ is an input schema, not copied application/example code.
This static check does not claim to execute StarUML's renderer or UML rules.
"""
import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('--metamodel', type=Path, required=True)
parser.add_argument('--model', type=Path, default=ROOT / 'ChengVault.mdj')
parser.add_argument('--output', type=Path, default=ROOT / 'schema-validation.json')
args = parser.parse_args()


def flatten(root):
    result = []
    def visit(value):
        if isinstance(value, dict):
            if '_id' in value:
                result.append(value)
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
    visit(root)
    return result


meta_bytes = args.metamodel.read_bytes()
meta = flatten(json.loads(meta_bytes.decode('utf-8-sig')))
classes = {x['_id']: x for x in meta if x['_type'] == 'UMLClass'}
names = {x['name']: x['_id'] for x in classes.values()}
bases, fields, ref_types = defaultdict(list), defaultdict(set), defaultdict(dict)
for key, value in classes.items():
    fields[key].update(attribute['name'] for attribute in value.get('attributes', []))
for item in meta:
    if item['_type'] == 'UMLGeneralization':
        bases[item['source']['$ref']].append(item['target']['$ref'])
    if item['_type'] == 'UMLAssociation':
        # A named association end is a property of the opposite classifier.
        for source, target in ((item['end1'], item['end2']), (item['end2'], item['end1'])):
            owner = source.get('reference', {}).get('$ref')
            if owner in classes and target.get('name'):
                fields[owner].add(target['name'])
                ref_types[owner][target['name']] = target.get('reference', {}).get('$ref')


def ancestors(key):
    result, pending = {key}, [key]
    while pending:
        for parent in bases[pending.pop()]:
            if parent not in result:
                result.add(parent)
                pending.append(parent)
    return result


model_bytes = args.model.read_bytes()
model = flatten(json.loads(model_bytes.decode('utf-8-sig')))
by_id = {item['_id']: item for item in model}
assert len(by_id) == len(model), 'Duplicate model IDs'
unknown_types, unknown_fields, wrong_refs, unresolved = [], [], [], []
checked = 0
for item in model:
    kind = item['_type']
    if kind not in names:
        unknown_types.append(kind)
        continue
    allowed, typed = {'_type'}, {}
    for ancestor in ancestors(names[kind]):
        allowed.update(fields[ancestor])
        typed.update(ref_types[ancestor])
    for field, value in item.items():
        if field not in allowed:
            unknown_fields.append({'type': kind, 'field': field, 'id': item['_id']})
        expected = typed.get(field)
        for candidate in (value if isinstance(value, list) else [value]):
            target = None
            if isinstance(candidate, dict) and '$ref' in candidate:
                target = by_id.get(candidate['$ref'])
                if target is None:
                    unresolved.append(candidate['$ref'])
            elif isinstance(candidate, dict) and '_type' in candidate:
                target = candidate
            if target is not None and expected in classes:
                checked += 1
                actual = names.get(target['_type'])
                if actual is None or expected not in ancestors(actual):
                    wrong_refs.append({'type': kind, 'field': field,
                                       'expected': classes[expected]['name'], 'actual': target['_type']})

report = {
    'officialMetamodelUrl': 'https://raw.githubusercontent.com/staruml/staruml-samples/main/StarUML-metamodel.mdj',
    'officialMetamodelSha256': hashlib.sha256(meta_bytes).hexdigest(),
    'modelSha256': hashlib.sha256(model_bytes).hexdigest(),
    'modelTypes': len({item['_type'] for item in model}),
    'modelsAndViews': len(model), 'checkedAssociationFieldTypes': checked,
    'unknownTypes': unknown_types, 'unknownFields': unknown_fields,
    'wrongReferenceTypes': wrong_refs, 'unresolvedReferences': unresolved,
    'passed': not (unknown_types or unknown_fields or wrong_refs or unresolved),
    'nativeStarUmlOpenVerified': False,
    'limits': ['Static JSON/metamodel-field/reference checks only.',
               'Does not run the native renderer or every UML well-formedness rule.']}
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report, ensure_ascii=True))
raise SystemExit(0 if report['passed'] else 1)
