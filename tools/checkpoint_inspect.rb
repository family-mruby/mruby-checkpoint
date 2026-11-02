# Show a checkpoint image (format v1) in readable form.
#
#   ruby tools/checkpoint_inspect.rb IMAGE [--iseq]       every record of every section
#   ruby tools/checkpoint_inspect.rb IMAGE --json [--no-code] [--pretty]
#                                                         the inspect JSON (doc/inspect_json.md)
#   ruby tools/checkpoint_inspect.rb IMAGE --stack        the call stacks of the root, the
#                                                         Tasks and the Fibers, as backtraces,
#                                                         with the locals
#   ruby tools/checkpoint_inspect.rb IMAGE --objects [--depth N] [--from PATH|#ID]
#                                                         the object graph from the roots
#   ruby tools/checkpoint_inspect.rb IMAGE --tasks        the scheduler, the Tasks, the contexts
#   ruby tools/checkpoint_inspect.rb IMAGE --html OUT.html
#                                                         one self-contained page of all of it
#   ruby tools/checkpoint_inspect.rb diff A B [--json] [--html OUT.html]
#                                                         what changed from image A to image B
#
# All views are made from the one reader, tools/lib/checkpoint_image.rb.
# A broken image prints where it breaks (byte offset and record) and exits
# with status 1; with --json the part read so far is printed with "error".

require 'json'
require_relative 'lib/checkpoint_image'
require_relative 'lib/checkpoint_text'
require_relative 'lib/checkpoint_diff'
require_relative 'lib/checkpoint_html'

USAGE = <<~U
  usage: checkpoint_inspect.rb IMAGE [--iseq | --json [--no-code] [--pretty] | --stack |
                                      --objects [--depth N] [--from PATH|#ID] | --tasks | --html OUT]
         checkpoint_inspect.rb diff A B [--json [--pretty] | --html OUT]
U

args = ARGV.dup
opt = {}
files = []
until args.empty?
  a = args.shift
  case a
  when '--iseq', '--json', '--no-code', '--pretty', '--stack', '--objects', '--tasks' then opt[a[2..].to_sym] = true
  when '--depth' then opt[:depth] = Integer(args.shift || abort(USAGE))
  when '--from' then opt[:from] = args.shift || abort(USAGE)
  when '--html' then opt[:html] = args.shift || abort(USAGE)
  when '-h', '--help' then puts USAGE; exit 0
  when /\A--/ then abort "unknown option #{a}\n#{USAGE}"
  else files << a
  end
end

def load(path, partial)
  CheckpointImage.read_file(path, partial: partial)
rescue CheckpointImage::FormatError => e
  warn "#{path}: #{e.message}"
  exit 1
rescue SystemCallError => e
  warn "#{path}: #{e.message}"
  exit 2
end

def emit_json(obj, pretty)
  puts pretty ? JSON.pretty_generate(obj) : JSON.generate(obj)
end

if files.first == 'diff'
  abort USAGE unless files.size == 3
  a = load(files[1], false)
  b = load(files[2], false)
  d = CheckpointDiff.diff(a, b)
  if opt[:html]
    File.write(opt[:html], CheckpointHtml.page([CheckpointImage.without_code(a), CheckpointImage.without_code(b)], d))
    puts "wrote #{opt[:html]} (#{File.size(opt[:html])} bytes)"
  elsif opt[:json]
    emit_json(d, opt[:pretty])
  else
    print CheckpointDiff.text(d)
  end
  exit 0
end

abort USAGE unless files.size == 1
doc = load(files[0], opt[:json])
if opt[:json]
  emit_json(opt[:'no-code'] ? CheckpointImage.without_code(doc) : doc, opt[:pretty])
  exit(doc['error'] ? 1 : 0)
elsif opt[:html]
  File.write(opt[:html], CheckpointHtml.page([CheckpointImage.without_code(doc)], nil))
  puts "wrote #{opt[:html]} (#{File.size(opt[:html])} bytes)"
elsif opt[:stack] then print CheckpointText.stack(doc)
elsif opt[:objects] then print CheckpointText.objects(doc, depth: opt[:depth] || 2, from: opt[:from])
elsif opt[:tasks] then print CheckpointText.tasks(doc)
else print CheckpointText.dump(doc, iseq: opt[:iseq])
end
