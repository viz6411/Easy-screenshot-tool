"""Board is the carrier of work: read it back through the API, move cards, never guess.

Run:
  python tools/board_sync.py report                every card: number, status, milestone, labels
  python tools/board_sync.py move 7 Development     set one card's Status column
  python tools/board_sync.py fill 1                 milestone 1: empty Status -> Ready

Column names and option ids are read from the board itself; nothing here assumes a column exists.
A readiness report is only trustworthy when it was read from the API, not from what we intended.
"""
import json
import os
import sys
import urllib.request

OWNER = "viz6411"
REPO = "Easy-screenshot-tool"
PROJECT = 3

FIELDS_Q = """
query($owner: String!, $repo: String!, $number: Int!) {
  repository(owner: $owner, name: $repo) {
    projectV2(number: $number) {
      id
      title
      fields(first: 40) {
        nodes {
          __typename
          ... on ProjectV2SingleSelectField { id name options { id name } }
        }
      }
    }
  }
}
"""

ITEMS_Q = """
query($owner: String!, $repo: String!, $number: Int!, $cursor: String) {
  repository(owner: $owner, name: $repo) {
    projectV2(number: $number) {
      items(first: 50, after: $cursor) {
        pageInfo { hasNextPage endCursor }
        nodes {
          id
          type
          content {
            ... on Issue { number title state milestone { title } labels(first: 12) { nodes { name } } }
          }
          fieldValues(first: 20) {
            nodes {
              ... on ProjectV2ItemFieldSingleSelectValue { name field { ... on ProjectV2SingleSelectField { name } } }
              ... on ProjectV2ItemFieldIterationValue { title }
            }
          }
        }
      }
    }
  }
}
"""

MOVE_Q = """
mutation($projectId: ID!, $itemId: ID!, $fieldId: ID!, $optionId: String!) {
  updateProjectV2ItemFieldValue(input: { projectId: $projectId, itemId: $itemId,
    fieldId: $fieldId, value: { singleSelectOptionId: $optionId } }) {
    projectV2Item { id }
  }
}
"""

ISSUE_Q = """
query($owner: String!, $repo: String!, $number: Int!) {
  repository(owner: $owner, name: $repo) { issue(number: $number) { id title } }
}
"""

ADD_Q = """
mutation($projectId: ID!, $contentId: ID!) {
  addProjectV2ItemById(input: { projectId: $projectId, contentId: $contentId }) { item { id } }
}
"""


def token():
    """The board needs the same credential gh already has; read it, never print it."""
    for name in ('GITHUB_TOKEN', 'GH_TOKEN'):
        value = os.environ.get(name, '').strip()
        if value:
            return value
    for path in (os.path.expandvars(r"%APPDATA%\GitHub\cli\config.yml"),
                 os.path.expanduser(r"~\.config\gh\hosts.yml")):
        if os.path.exists(path):
            for line in open(path, encoding='utf-8', errors='replace'):
                s = line.strip()
                if s.startswith('oauth_token:'):
                    value = s.split(':', 1)[1].strip().strip('"')
                    if value:
                        return value
    raise SystemExit('no gh token found - run: gh auth login')


def gql(query, **variables):
    body = json.dumps({'query': query, 'variables': variables}).encode()
    req = urllib.request.Request(
        'https://api.github.com/graphql', data=body,
        headers={'Authorization': 'Bearer ' + token(), 'Content-Type': 'application/json',
                 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req) as resp:
        data = json.loads(resp.read().decode('utf-8'))
    if 'errors' in data:
        raise SystemExit(json.dumps(data['errors'], ensure_ascii=False))
    return data['data']


def board():
    proj = gql(FIELDS_Q, owner=OWNER, repo=REPO, number=PROJECT)['repository']['projectV2']
    fields = {}
    for f in proj['fields']['nodes']:
        name = f.get('name')
        if not name:
            continue
        fields[name.lower()] = f
    return proj, fields


def items():
    out, cursor = [], None
    while True:
        data = gql(ITEMS_Q, owner=OWNER, repo=REPO, number=PROJECT, cursor=cursor)
        page = data['repository']['projectV2']['items']
        for node in page['nodes']:
            out.append(node)
        if not page['pageInfo']['hasNextPage']:
            return out
        cursor = page['pageInfo']['endCursor']


def status_of(node):
    for v in node['fieldValues']['nodes']:
        field = v.get('field') or {}
        if (field.get('name') or '').lower() == 'status':
            return v.get('name') or ''
    return ''


def report():
    proj, fields = board()
    status = fields.get('status')
    print('project: %s (status field: %s)' % (proj['title'], (status or {}).get('name', 'ABSENT')))
    if status and status.get('options'):
        print('columns: %s' % ', '.join(o['name'] for o in status['options']))
    rows = items()
    print('%-6s %-7s %-28s %s' % ('card', 'status', 'milestone', 'labels'))
    for node in sorted(rows, key=lambda n: (n.get('content') or {}).get('number') or 0):
        c = node.get('content') or {}
        if not c:
            continue
        labels = ','.join(l['name'] for l in (c.get('labels') or {}).get('nodes', []))
        print('#%-5s %-7s %-28s %s' % (c.get('number'), status_of(node) or '-',
                                       (c.get('milestone') or {}).get('title', ''), labels[:60]))
    missing = [c['content']['number'] for c in
               [n for n in rows if not status_of(n)] if c.get('content')]
    if missing:
        print('cards without Status: %s' % ', '.join('#%s' % n for n in missing))
    return 0


def move(number, column):
    proj, fields = board()
    status = fields.get('status')
    if not status or not status.get('options'):
        raise SystemExit('the board has no Status single-select field to move cards with')
    options = {o['name'].lower(): o['id'] for o in status['options']}
    if column.lower() not in options:
        raise SystemExit('no column %r; columns are: %s'
                         % (column, ', '.join(o['name'] for o in status['options'])))
    for node in items():
        c = node.get('content') or {}
        if c.get('number') == number:
            gql(MOVE_Q, projectId=proj['id'], itemId=node['id'], fieldId=status['id'],
                optionId=options[column.lower()])
            print('#%s -> %s' % (number, column))
            return 0
    raise SystemExit('card #%s is not on the board' % number)


def fill(milestone_number):
    proj, fields = board()
    status = fields.get('status')
    if not status or not status.get('options'):
        raise SystemExit('the board has no Status single-select field')
    options = {o['name'].lower(): o['id'] for o in status['options']}
    if 'ready' not in options:
        raise SystemExit('no Ready column')
    moved = 0
    for node in items():
        c = node.get('content') or {}
        mile = (c.get('milestone') or {}).get('title')
        if mile and str(milestone_number) in mile and not status_of(node):
            gql(MOVE_Q, projectId=proj['id'], itemId=node['id'], fieldId=status['id'],
                optionId=options['ready'])
            moved += 1
    print('cards placed in Ready: %d' % moved)
    return 0


def add(number):
    proj, _ = board()
    issue = gql(ISSUE_Q, owner=OWNER, repo=REPO, number=number)['repository']['issue']
    if not issue:
        raise SystemExit('issue #%s does not exist' % number)
    gql(ADD_Q, projectId=proj['id'], contentId=issue['id'])
    print('#%s added to the board' % number)
    return 0


def main():
    args = sys.argv[1:]
    if not args or args[0] == 'report':
        return report()
    if args[0] == 'add' and len(args) == 2:
        return add(int(args[1]))
    if args[0] == 'move' and len(args) == 3:
        return move(int(args[1]), args[2])
    if args[0] == 'fill' and len(args) == 2:
        return fill(int(args[1]))
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main())
