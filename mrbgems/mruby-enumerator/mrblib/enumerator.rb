##
# enumerator.rb Enumerator class
# See Copyright Notice in mruby.h

##
# A class which allows both internal and external iteration.
#
# An Enumerator can be created by the following methods.
# - {Kernel#to_enum}
# - {Kernel#enum_for}
# - {Enumerator#initialize Enumerator.new}
#
# Most methods have two forms: a block form where the contents
# are evaluated for each item in the enumeration, and a non-block form
# which returns a new Enumerator wrapping the iteration.
#
#       enumerator = %w(one two three).each
#       puts enumerator.class # => Enumerator
#
#       enumerator.each_with_object("foo") do |item, obj|
#         puts "#{obj}: #{item}"
#       end
#
#       # foo: one
#       # foo: two
#       # foo: three
#
#       enum_with_obj = enumerator.each_with_object("foo")
#       puts enum_with_obj.class # => Enumerator
#
#       enum_with_obj.each do |item, obj|
#         puts "#{obj}: #{item}"
#       end
#
#       # foo: one
#       # foo: two
#       # foo: three
#
# This allows you to chain Enumerators together. For example, you
# can map a list's elements to strings containing the index
# and the element as a string via:
#
#       puts %w[foo bar baz].map.with_index { |w, i| "#{i}:#{w}" }
#       # => ["0:foo", "1:bar", "2:baz"]
#
# An Enumerator can also be used as an external iterator.
# For example, Enumerator#next returns the next value of the iterator
# or raises StopIteration if the Enumerator is at the end.
#
#       e = [1,2,3].each   # returns an enumerator object.
#       puts e.next   # => 1
#       puts e.next   # => 2
#       puts e.next   # => 3
#       puts e.next   # raises StopIteration
#
# You can use this to implement an internal iterator as follows:
#
#       def ext_each(e)
#         while true
#           begin
#             vs = e.next_values
#           rescue StopIteration
#             return $!.result
#           end
#           y = yield(*vs)
#           e.feed y
#         end
#       end
#
#       o = Object.new
#
#       def o.each
#         puts yield
#         puts yield(1)
#         puts yield(1, 2)
#         3
#       end
#
#       # use o.each as an internal iterator directly.
#       puts o.each {|*x| puts x; [:b, *x] }
#       # => [], [:b], [1], [:b, 1], [1, 2], [:b, 1, 2], 3
#
#       # convert o.each to an external iterator for
#       # implementing an internal iterator.
#       puts ext_each(o.to_enum) {|*x| puts x; [:b, *x] }
#       # => [], [:b], [1], [:b, 1], [1, 2], [:b, 1, 2], 3
#
class Enumerator
  include Enumerable

  ##
  # @overload initialize(obj, method = :each, *args, **kwd)
  #
  # Creates a new Enumerator object, which can be used as an
  # Enumerable.
  #
  # In the first form, iteration is defined by the given block, in
  # which a "yielder" object, given as block parameter, can be used to
  # yield a value by calling the +yield+ method (aliased as +<<+):
  #
  #     fib = Enumerator.new do |y|
  #       a = b = 1
  #       loop do
  #         y << a
  #         a, b = b, a + b
  #       end
  #     end
  #
  #     p fib.take(10) # => [1, 1, 2, 3, 5, 8, 13, 21, 34, 55]
  #
  # In the second, deprecated, form, a generated Enumerator iterates over the
  # given object using the given method with the given arguments passed. This
  # form is left only for internal use.
  #
  # Use of this form is discouraged. Use Kernel#enum_for or Kernel#to_enum
  # instead.
  def initialize(obj=NONE, meth=:each, *args, **kwd, &block)
    if block
      obj = Generator.new(&block)
    elsif NONE.equal?(obj)
      raise ArgumentError, "wrong number of arguments (given 0, expected 1+)"
    end

    @obj = obj
    @meth = meth
    @args = args
    @kwd = kwd
    @fib = nil
    @dst = nil
    @lookahead = nil
    @feedvalue = nil
    @stop_exc = false
  end

  private def initialize_copy(obj)
    raise TypeError, "can't copy type #{obj.class}" unless obj.kind_of? Enumerator
    raise TypeError, "can't copy execution context" if obj.instance_eval{@fib}
    meth = args = kwd = fib = nil
    obj.instance_eval {
      obj = @obj
      meth = @meth
      args = @args
      kwd = @kwd
    }
    @obj = obj
    @meth = meth
    @args = args
    @kwd = kwd
    @fib = nil
    @lookahead = nil
    @feedvalue = nil
    self
  end

  ##
  # call-seq:
  #   e.with_index(offset = 0) {|(*args), idx| ... }
  #   e.with_index(offset = 0)
  #
  # Iterates the given block for each element with an index, which
  # starts from +offset+. If no block is given, returns a new Enumerator
  # that includes the index, starting from +offset+
  #
  # +offset+:: the starting index to use
  #
  def with_index(offset=0, &block)
    return to_enum :with_index, offset unless block

    if offset.nil?
      offset = 0
    else
      offset = offset.__to_int
    end

    n = offset - 1
    __enumerator_block_call do |*i|
      n += 1
      block.call i.__svalue, n
    end
  end

  ##
  # call-seq:
  #   e.each_with_index {|(*args), idx| ... }
  #   e.each_with_index
  #
  # Same as Enumerator#with_index(0), i.e. there is no starting offset.
  #
  # If no block is given, a new Enumerator is returned that includes the index.
  #
  def each_with_index(&block)
    with_index(0, &block)
  end

  ##
  # call-seq:
  #   e.each_with_object(obj) {|(*args), obj| ... }
  #   e.each_with_object(obj)
  #   e.with_object(obj) {|(*args), obj| ... }
  #   e.with_object(obj)
  #
  # Iterates the given block for each element with an arbitrary object, +obj+,
  # and returns +obj+
  #
  # If no block is given, returns a new Enumerator.
  #
  # @example
  #   to_three = Enumerator.new do |y|
  #     3.times do |x|
  #       y << x
  #     end
  #   end
  #
  #   to_three_with_string = to_three.with_object("foo")
  #   to_three_with_string.each do |x,string|
  #     puts "#{string}: #{x}"
  #   end
  #
  #   # => foo:0
  #   # => foo:1
  #   # => foo:2
  #
  def with_object(object, &block)
    return to_enum(:with_object, object) unless block

    __enumerator_block_call do |i|
      block.call [i,object]
    end
    object
  end

  def inspect
    if @args && @args.size > 0
      args = @args.join(", ")
      "#<#{self.class}: #{@obj.inspect}:#{@meth}(#{args})>"
    else
      "#<#{self.class}: #{@obj.inspect}:#{@meth}>"
    end
  end

  def size
    if @size
      @size
    elsif @obj.respond_to?(:size)
      @obj.size
    end
  end

  ##
  # call-seq:
  #   enum.each { |elm| block }                    -> obj
  #   enum.each                                    -> enum
  #   enum.each(*appending_args) { |elm| block }   -> obj
  #   enum.each(*appending_args)                   -> an_enumerator
  #
  # Iterates over the block according to how this Enumerator was constructed.
  # If no block and no arguments are given, returns self.
  #
  # === Examples
  #
  #   Array.new(3)                     #=> [nil, nil, nil]
  #   Array.new(3) { |i| i }           #=> [0, 1, 2]
  #   Array.to_enum(:new, 3).to_a      #=> [0, 1, 2]
  #   Array.to_enum(:new).each(3).to_a #=> [0, 1, 2]
  #
  #   obj = Object.new
  #
  #   def obj.each_arg(a, b=:b, *rest)
  #     yield a
  #     yield b
  #     yield rest
  #     :method_returned
  #   end
  #
  #   enum = obj.to_enum :each_arg, :a, :x
  #
  #   enum.each.to_a                  #=> [:a, :x, []]
  #   enum.each.equal?(enum)          #=> true
  #   enum.each { |elm| elm }         #=> :method_returned
  #
  #   enum.each(:y, :z).to_a          #=> [:a, :x, [:y, :z]]
  #   enum.each(:y, :z).equal?(enum)  #=> false
  #   enum.each(:y, :z) { |elm| elm } #=> :method_returned
  #
  def each(*argv, &block)
    obj = self
    if 0 < argv.length
      obj = self.dup
      args = obj.instance_eval{@args}
      if !args.empty?
        args = args.dup
        args.concat argv
      else
        args = argv.dup
      end
      obj.instance_eval{@args = args}
    end
    return obj unless block
    __enumerator_block_call(&block)
  end

  def __enumerator_block_call(&block)
    @obj.__send__ @meth, *@args, **@kwd, &block
  end
  private :__enumerator_block_call

  ##
  # call-seq:
  #   e.next   -> object
  #
  # Returns the next object in the enumerator, and move the internal position
  # forward. When the position reached at the end, StopIteration is raised.
  #
  # === Example
  #
  #   a = [1,2,3]
  #   e = a.to_enum
  #   p e.next   #=> 1
  #   p e.next   #=> 2
  #   p e.next   #=> 3
  #   p e.next   #raises StopIteration
  #
  # Note that enumeration sequence by +next+ does not affect other non-external
  # enumeration methods, unless the underlying iteration methods itself has
  # side-effect
  #
  def next
    next_values.__svalue
  end

  ##
  # call-seq:
  #   e.next_values   -> array
  #
  # Returns the next object as an array in the enumerator, and move the
  # internal position forward. When the position reached at the end,
  # StopIteration is raised.
  #
  # This method can be used to distinguish <code>yield</code> and <code>yield
  # nil</code>.
  #
  # === Example
  #
  #   o = Object.new
  #   def o.each
  #     yield
  #     yield 1
  #     yield 1, 2
  #     yield nil
  #     yield [1, 2]
  #   end
  #   e = o.to_enum
  #   p e.next_values
  #   p e.next_values
  #   p e.next_values
  #   p e.next_values
  #   p e.next_values
  #   e = o.to_enum
  #   p e.next
  #   p e.next
  #   p e.next
  #   p e.next
  #   p e.next
  #
  #   ## yield args       next_values      next
  #   #  yield            []               nil
  #   #  yield 1          [1]              1
  #   #  yield 1, 2       [1, 2]           [1, 2]
  #   #  yield nil        [nil]            nil
  #   #  yield [1, 2]     [[1, 2]]         [1, 2]
  #
  # Note that +next_values+ does not affect other non-external enumeration
  # methods unless underlying iteration method itself has side-effect
  #
  def next_values
    if @lookahead
      vs = @lookahead
      @lookahead = nil
      return vs
    end
    raise @stop_exc if @stop_exc

    curr = Fiber.current

    if !@fib || !@fib.alive?
      @dst = curr
      @fib = Fiber.new do
        result = each do |*args|
          feedvalue = nil
          Fiber.yield args
          if @feedvalue
            feedvalue = @feedvalue
            @feedvalue = nil
          end
          feedvalue
        end
        @stop_exc = StopIteration.new "iteration reached an end"
        @stop_exc.result = result
        Fiber.yield nil
      end
      @lookahead = nil
    end

    vs = @fib.resume curr
    if @stop_exc
      @fib = nil
      @dst = nil
      @lookahead = nil
      @feedvalue = nil
      raise @stop_exc
    end
    vs
  end

  ##
  # call-seq:
  #   e.peek   -> object
  #
  # Returns the next object in the enumerator, but doesn't move the internal
  # position forward. If the position is already at the end, StopIteration
  # is raised.
  #
  # === Example
  #
  #   a = [1,2,3]
  #   e = a.to_enum
  #   p e.next   #=> 1
  #   p e.peek   #=> 2
  #   p e.peek   #=> 2
  #   p e.peek   #=> 2
  #   p e.next   #=> 2
  #   p e.next   #=> 3
  #   p e.next   #raises StopIteration
  #
  def peek
    peek_values.__svalue
  end

  ##
  # call-seq:
  #   e.peek_values   -> array
  #
  # Returns the next object as an array, similar to Enumerator#next_values, but
  # doesn't move the internal position forward. If the position is already at
  # the end, StopIteration is raised.
  #
  # === Example
  #
  #   o = Object.new
  #   def o.each
  #     yield
  #     yield 1
  #     yield 1, 2
  #   end
  #   e = o.to_enum
  #   p e.peek_values    #=> []
  #   e.next
  #   p e.peek_values    #=> [1]
  #   p e.peek_values    #=> [1]
  #   e.next
  #   p e.peek_values    #=> [1, 2]
  #   e.next
  #   p e.peek_values    # raises StopIteration
  #
  def peek_values
    if @lookahead.nil?
      @lookahead = next_values
    end
    @lookahead.dup
  end

  ##
  # call-seq:
  #   e.rewind   -> e
  #
  # Rewinds the enumeration sequence to the beginning.
  #
  # If the enclosed object responds to a "rewind" method, it is called.
  #
  def rewind
    @obj.rewind if @obj.respond_to? :rewind
    @fib = nil
    @dst = nil
    @lookahead = nil
    @feedvalue = nil
    @stop_exc = false
    self
  end

  ##
  # call-seq:
  #   e.feed obj   -> nil
  #
  # Sets the value to be returned by the next yield inside +e+.
  #
  # If the value is not set, the yield returns nil.
  #
  # This value is cleared after being yielded.
  #
  #   # Array#map passes the array's elements to "yield" and collects the
  #   # results of "yield" as an array.
  #   # Following example shows that "next" returns the passed elements and
  #   # values passed to "feed" are collected as an array which can be
  #   # obtained by StopIteration#result.
  #   e = [1,2,3].map
  #   p e.next           #=> 1
  #   e.feed "a"
  #   p e.next           #=> 2
  #   e.feed "b"
  #   p e.next           #=> 3
  #   e.feed "c"
  #   begin
  #     e.next
  #   rescue StopIteration
  #     p $!.result      #=> ["a", "b", "c"]
  #   end
  #
  #   o = Object.new
  #   def o.each
  #     x = yield         # (2) blocks
  #     p x               # (5) => "foo"
  #     x = yield         # (6) blocks
  #     p x               # (8) => nil
  #     x = yield         # (9) blocks
  #     p x               # not reached w/o another e.next
  #   end
  #
  #   e = o.to_enum
  #   e.next              # (1)
  #   e.feed "foo"        # (3)
  #   e.next              # (4)
  #   e.next              # (7)
  #                       # (10)
  #
  def feed(value)
    raise TypeError, "feed value already set" if @feedvalue
    @feedvalue = value
    nil
  end

  # just for internal
  class Generator
    include Enumerable
    def initialize(&block)
      raise TypeError, "wrong argument type #{self.class} (expected Proc)" unless block.kind_of? Proc

      @proc = block
    end

    def each(*args, &block)
      args.unshift Yielder.new(&block)
      @proc.call(*args)
    end
  end

  # just for internal
  class Yielder
    def initialize(&block)
      raise LocalJumpError, "no block given" unless block

      @proc = block
    end

    def yield(*args)
      @proc.call(*args)
    end

    def << *args
      self.yield(*args)
      self
    end
  end

  ##
  # call-seq:
  #    Enumerator.produce(initial = nil) { |val| } -> enumerator
  #
  # Creates an infinite enumerator from any block, just called over and
  # over. Result of the previous iteration is passed to the next one.
  # If +initial+ is provided, it is passed to the first iteration, and
  # becomes the first element of the enumerator; if it is not provided,
  # first iteration receives +nil+, and its result becomes first
  # element of the iterator.
  #
  # Raising StopIteration from the block stops an iteration.
  #
  # Examples of usage:
  #
  #   Enumerator.produce(1, &:succ)   # => enumerator of 1, 2, 3, 4, ...
  #
  #   Enumerator.produce { rand(10) } # => infinite random number sequence
  #
  #   ancestors = Enumerator.produce(node) { |prev| node = prev.parent or raise StopIteration }
  #   enclosing_section = ancestors.find { |n| n.type == :section }
  def Enumerator.produce(init=NONE, &block)
    raise ArgumentError, "no block given" if block.nil?
    Enumerator.new do |y|
      if NONE.equal?(init)
        val = nil
      else
        val = init
        y.yield(val)
      end
      begin
        while true
          y.yield(val = block.call(val))
        end
      rescue StopIteration
        # do nothing
      end
    end
  end
end

module Kernel
  ##
  # call-seq:
  #   obj.to_enum(method = :each, *args)                 -> enum
  #   obj.enum_for(method = :each, *args)                -> enum
  #
  # Creates a new Enumerator which will enumerate by calling +method+ on
  # +obj+, passing +args+ if any.
  #
  # === Examples
  #
  #   str = "xyz"
  #
  #   enum = str.enum_for(:each_byte)
  #   enum.each { |b| puts b }
  #   # => 120
  #   # => 121
  #   # => 122
  #
  #   # protect an array from being modified by some_method
  #   a = [1, 2, 3]
  #   some_method(a.to_enum)
  #
  # It is typical to call to_enum when defining methods for
  # a generic Enumerable, in case no block is passed.
  #
  # Here is such an example with parameter passing:
  #
  #     module Enumerable
  #       # a generic method to repeat the values of any enumerable
  #       def repeat(n)
  #         raise ArgumentError, "#{n} is negative!" if n < 0
  #         unless block_given?
  #           return to_enum(__callee__, n) do # __callee__ is :repeat here
  #         end
  #         each do |*val|
  #           n.times { yield *val }
  #         end
  #       end
  #     end
  #
  #     %i[hello world].repeat(2) { |w| puts w }
  #       # => Prints 'hello', 'hello', 'world', 'world'
  #     enum = (1..14).repeat(3)
  #       # => returns an Enumerator when called without a block
  #     enum.first(4) # => [1, 1, 1, 2]
  #
  def to_enum(meth=:each, *args, **kwd)
    Enumerator.new self, meth, *args, **kwd
  end
  alias enum_for to_enum
end

module Enumerable
  # use Enumerator to use infinite sequence
  def zip(*args, &block)
    args = args.map do |a|
      if a.respond_to?(:each)
        a.to_enum(:each)
      else
        raise TypeError, "wrong argument type #{a.class} (must respond to :each)"
      end
    end

    result = block ? nil : []

    each do |*val|
      tmp = [val.__svalue]
      args.each do |arg|
        v = if arg.nil?
          nil
        else
          begin
            arg.next
          rescue StopIteration
            nil
          end
        end
        tmp.push(v)
      end
      if result.nil?
        block.call(tmp)
      else
        result.push(tmp)
      end
    end

    result
  end

  ##
  #  call-seq:
  #    enum.chunk                 -> enumerator
  #    enum.chunk { |arr| block } -> enumerator
  #
  #  Each element in the returned enumerator is a 2-element array consisting of:
  #
  #  - A value returned by the block.
  #  - An array ("chunk") containing the element for which that value was returned,
  #    and all following elements for which the block returned the same value:
  #
  #  So that:
  #
  #  - Each block return value that is different from its predecessor
  #    begins a new chunk.
  #  - Each block return value that is the same as its predecessor
  #    continues the same chunk.
  #
  #  Example:
  #
  #     e = (0..10).chunk {|i| (i / 3).floor } # => #<Enumerator: ...>
  #     # The enumerator elements.
  #     e.next # => [0, [0, 1, 2]]
  #     e.next # => [1, [3, 4, 5]]
  #     e.next # => [2, [6, 7, 8]]
  #     e.next # => [3, [9, 10]]
  #
  #  You can use the special symbol <tt>:_alone</tt> to force an element
  #  into its own separate chuck:
  #
  #     a = [0, 0, 1, 1]
  #     e = a.chunk{|i| i.even? ? :_alone : true }
  #     e.to_a # => [[:_alone, [0]], [:_alone, [0]], [true, [1, 1]]]
  #
  #  You can use the special symbol <tt>:_separator</tt> or +nil+
  #  to force an element to be ignored (not included in any chunk):
  #
  #     a = [0, 0, -1, 1, 1]
  #     e = a.chunk{|i| i < 0 ? :_separator : true }
  #     e.to_a # => [[true, [0, 0]], [true, [1, 1]]]
  def chunk(&block)
    return to_enum :chunk unless block

    enum = self
    Enumerator.new do |y|
      last_value, arr = nil, []
      enum.each do |element|
        value = block.call(element)
        case value
        when :_alone
          y.yield [last_value, arr] if arr.size > 0
          y.yield [value, [element]]
          last_value, arr = nil, []
        when :_separator, nil
          y.yield [last_value, arr] if arr.size > 0
          last_value, arr = nil, []
        when last_value
          arr << element
        else
          raise 'symbols beginning with an underscore are reserved' if value.is_a?(Symbol) && value.to_s[0] == '_'
          y.yield [last_value, arr] if arr.size > 0
          last_value, arr = value, [element]
        end
      end
      y.yield [last_value, arr] if arr.size > 0
    end
  end


  ##
  #  call-seq:
  #     enum.chunk_while {|elt_before, elt_after| bool } -> an_enumerator
  #
  # Creates an enumerator for each chunked elements.
  # The beginnings of chunks are defined by the block.
  #
  # This method splits each chunk using adjacent elements,
  # _elt_before_ and _elt_after_,
  # in the receiver enumerator.
  # This method split chunks between _elt_before_ and _elt_after_ where
  # the block returns <code>false</code>.
  #
  # The block is called the length of the receiver enumerator minus one.
  #
  # The result enumerator yields the chunked elements as an array.
  # So +each+ method can be called as follows:
  #
  #   enum.chunk_while { |elt_before, elt_after| bool }.each { |ary| ... }
  #
  # Other methods of the Enumerator class and Enumerable module,
  # such as +to_a+, +map+, etc., are also usable.
  #
  # For example, one-by-one increasing subsequence can be chunked as follows:
  #
  #   a = [1,2,4,9,10,11,12,15,16,19,20,21]
  #   b = a.chunk_while {|i, j| i+1 == j }
  #   p b.to_a #=> [[1, 2], [4], [9, 10, 11, 12], [15, 16], [19, 20, 21]]
  #   c = b.map {|a| a.length < 3 ? a : "#{a.first}-#{a.last}" }
  #   p c #=> [[1, 2], [4], "9-12", [15, 16], "19-21"]
  #   d = c.join(",")
  #   p d #=> "1,2,4,9-12,15,16,19-21"
  #
  # Increasing (non-decreasing) subsequence can be chunked as follows:
  #
  #   a = [0, 9, 2, 2, 3, 2, 7, 5, 9, 5]
  #   p a.chunk_while {|i, j| i <= j }.to_a
  #  #=> [[0, 9], [2, 2, 3], [2, 7], [5, 9], [5]]
  #
  # Adjacent evens and odds can be chunked as follows:
  # (Enumerable#chunk is another way to do it.)
  #
  #   a = [7, 5, 9, 2, 0, 7, 9, 4, 2, 0]
  #   p a.chunk_while {|i, j| i.even? == j.even? }.to_a
  #   #=> [[7, 5, 9], [2, 0], [7, 9], [4, 2, 0]]
  #
  # Enumerable#slice_when does the same, except splitting when the block
  # returns <code>true</code> instead of <code>false</code>.
  #
  def chunk_while(&block)
    enum = self
    Enumerator.new do |y|
      n = 0
      last_value, arr = nil, []
      enum.each do |element|
        if n > 0
          unless block.call(last_value, element)
            y.yield arr
            arr = []
          end
        end
        arr.push(element)
        n += 1
        last_value = element
      end
      y.yield arr if arr.size > 0
    end
  end
end
