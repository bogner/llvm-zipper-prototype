# summary provider for NSSet
import lldb
import ctypes
import objc_runtime
import metrics
import CFBag

statistics = metrics.Metrics()
statistics.add_metric('invalid_isa')
statistics.add_metric('invalid_pointer')
statistics.add_metric('unknown_class')
statistics.add_metric('code_notrun')

# despite the similary to synthetic children providers, these classes are not
# trying to provide anything but the port number of an NSMachPort, so they need not
# obey the interface specification for synthetic children providers
class NSCFSet_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		if not(self.sys_params.types_cache.NSUInteger):
			if self.sys_params.is_64_bit:
				self.sys_params.types_cache.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedLong)
			else:
				self.sys_params.types_cache.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedInt)
		self.update();

	def update(self):
		self.adjust_for_architecture();

	# one pointer is the ISA
	# then we have one other internal pointer, plus
	# 4 bytes worth of flags. hence, these values
	def offset(self):
		if self.sys_params.is_64_bit:
			return 20
		else:
			return 12

	def count(self):
		vcount = self.valobj.CreateChildAtOffset("count",
							self.offset(),
							self.sys_params.types_cache.NSUInteger)
		return vcount.GetValueAsUnsigned(0)


class NSSetUnknown_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		self.update();

	def update(self):
		self.adjust_for_architecture();

	def count(self):
		stream = lldb.SBStream()
		self.valobj.GetExpressionPath(stream)
		expr = "(int)[" + stream.GetData() + " count]"
		num_children_vo = self.valobj.CreateValueFromExpression("count",expr);
		return num_children_vo.GetValueAsUnsigned(0)

class NSSetI_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		if not(self.sys_params.types_cache.NSUInteger):
			if self.sys_params.is_64_bit:
				self.sys_params.types_cache.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedLong)
			else:
				self.sys_params.types_cache.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedInt)
		self.update();

	def update(self):
		self.adjust_for_architecture();

	# we just need to skip the ISA and the count immediately follows
	def offset(self):
		return self.sys_params.pointer_size

	def count(self):
		num_children_vo = self.valobj.CreateChildAtOffset("count",
							self.offset(),
							self.sys_params.types_cache.NSUInteger)
		value = num_children_vo.GetValueAsUnsigned(0)
		if value != None:
			# the MSB on immutable sets seems to be taken by some other data
			# not sure if it is a bug or some weird sort of feature, but masking it out
			# gets the count right (unless, of course, someone's dictionaries grow
			#                       too large - but I have not tested this)
			if self.sys_params.is_64_bit:
				value = value & ~0xFF00000000000000
			else:
				value = value & ~0xFF000000
		return value

class NSSetM_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		if not(self.sys_params.types_cache.NSUInteger):
			if self.sys_params.is_64_bit:
				self.sys_params.types_cache.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedLong)
			else:
				self.sys_params.types_cache.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedInt)
		self.update();

	def update(self):
		self.adjust_for_architecture();

	# we just need to skip the ISA and the count immediately follows
	def offset(self):
		return self.sys_params.pointer_size

	def count(self):
		num_children_vo = self.valobj.CreateChildAtOffset("count",
							self.offset(),
							self.sys_params.types_cache.NSUInteger)
		return num_children_vo.GetValueAsUnsigned(0)


class NSCountedSet_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		if not (self.sys_params.types_cache.voidptr):
			self.sys_params.types_cache.voidptr = self.valobj.GetType().GetBasicType(lldb.eBasicTypeVoid).GetPointerType()
		self.update();

	def update(self):
		self.adjust_for_architecture();

	# an NSCountedSet is implemented using a CFBag whose pointer just follows the ISA
	def offset(self):
		return self.sys_params.pointer_size

	def count(self):
		cfbag_vo = self.valobj.CreateChildAtOffset("bag_impl",
							self.offset(),
							self.sys_params.types_cache.voidptr)
		return CFBag.CFBagRef_SummaryProvider(cfbag_vo,self.sys_params).length()


def GetSummary_Impl(valobj):
	global statistics
	class_data = objc_runtime.ObjCRuntime(valobj)
	if class_data.is_valid() == False:
		statistics.metric_hit('invalid_pointer',valobj)
		wrapper = None
		return
	class_data = class_data.read_class_data()
	if class_data.is_valid() == False:
		statistics.metric_hit('invalid_isa',valobj)
		wrapper = None
		return
	if class_data.is_kvo():
		class_data = class_data.get_superclass()
	if class_data.is_valid() == False:
		statistics.metric_hit('invalid_isa',valobj)
		wrapper = None
		return
	
	name_string = class_data.class_name()
	if name_string == '__NSCFSet':
		wrapper = NSCFSet_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('code_notrun',valobj)
	elif name_string == '__NSSetI':
		wrapper = NSSetI_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('code_notrun',valobj)
	elif name_string == '__NSSetM':
		wrapper = NSSetM_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('code_notrun',valobj)
	elif name_string == 'NSCountedSet':
		wrapper = NSCountedSet_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('code_notrun',valobj)
	else:
		wrapper = NSSetUnknown_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('unknown_class',str(valobj) + " seen as " + name_string)
	return wrapper;


def NSSet_SummaryProvider (valobj,dict):
	provider = GetSummary_Impl(valobj);
	if provider != None:
	    #try:
	    summary = provider.count();
	    #except:
	    #    summary = None
	    if summary == None:
	        summary = 'no valid set here'
	    else:
	        summary = str(summary) + (' objects' if summary > 1 else ' object')
	    return summary
	return ''

def NSSet_SummaryProvider2 (valobj,dict):
	provider = GetSummary_Impl(valobj);
	if provider != None:
		try:
			summary = provider.count();
		except:
			summary = None
		# for some reason, one needs to clear some bits for the count returned
		# to be correct when using directly CF*SetRef as compared to NS*Set
		# this only happens on 64bit, and the bit mask was derived through
		# experimentation (if counts start looking weird, then most probably
		#                  the mask needs to be changed)
		if summary == None:
			summary = 'no valid set here'
		else:
			if provider.sys_params.is_64_bit:
				summary = summary & ~0x1fff000000000000
		 	summary = '@"' + str(summary) + (' values"' if summary > 1 else ' value"')
		return summary
	return ''


def __lldb_init_module(debugger,dict):
	debugger.HandleCommand("type summary add -F NSSet.NSSet_SummaryProvider NSSet")
	debugger.HandleCommand("type summary add -F NSSet.NSSet_SummaryProvider2 CFSetRef CFMutableSetRef")
