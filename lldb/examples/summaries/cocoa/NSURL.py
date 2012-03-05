# summary provider for NSURL
import lldb
import ctypes
import objc_runtime
import metrics
import CFString

statistics = metrics.Metrics()
statistics.add_metric('invalid_isa')
statistics.add_metric('invalid_pointer')
statistics.add_metric('unknown_class')
statistics.add_metric('code_notrun')

# despite the similary to synthetic children providers, these classes are not
# trying to provide anything but a summary for an NSURL, so they need not
# obey the interface specification for synthetic children providers
class NSURLKnown_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		if not(self.sys_params.types_cache.NSString):
			self.sys_params.types_cache.NSString = self.valobj.GetTarget().FindFirstType('NSString').GetPointerType()
		if not(self.sys_params.types_cache.NSURL):
			self.sys_params.types_cache.NSURL = self.valobj.GetTarget().FindFirstType('NSURL').GetPointerType()
		self.update();

	def update(self):
		self.adjust_for_architecture();

	# one pointer is the ISA
	# then there is one more pointer and 8 bytes of plain data
	# (which are also present on a 32-bit system)
	# plus another pointer, and then the real data
	def offset_text(self):
		return 24 if self.sys_params.is_64_bit else 16
	def offset_base(self):
		return self.offset_text()+self.sys_params.pointer_size

	def url_text(self):
		text = self.valobj.CreateChildAtOffset("text",
							self.offset_text(),
							self.sys_params.types_cache.NSString)
		base = self.valobj.CreateChildAtOffset("base",
							self.offset_base(),
							self.sys_params.types_cache.NSURL)
		my_string = CFString.CFString_SummaryProvider(text,None)
		if base.GetValueAsUnsigned(0) != 0:
			my_string = my_string + " (base path: " + NSURL_SummaryProvider(base,None) + ")"
		return my_string


class NSURLUnknown_SummaryProvider:
	def adjust_for_architecture(self):
		pass

	def __init__(self, valobj, params):
		self.valobj = valobj;
		self.sys_params = params
		self.update()

	def update(self):
		self.adjust_for_architecture();

	def url_text(self):
		stream = lldb.SBStream()
		self.valobj.GetExpressionPath(stream)
		url_text_vo = self.valobj.CreateValueFromExpression("url","(NSString*)[" + stream.GetData() + " description]");
		return CFString.CFString_SummaryProvider(url_text_vo,None)


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
	if name_string == 'NSURL':
		wrapper = NSURLKnown_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('code_notrun',valobj)
	else:
		wrapper = NSURLUnknown_SummaryProvider(valobj, class_data.sys_params)
		statistics.metric_hit('unknown_class',str(valobj) + " seen as " + name_string)
	return wrapper;

def NSURL_SummaryProvider (valobj,dict):
	provider = GetSummary_Impl(valobj);
	if provider != None:
	    try:
	        summary = provider.url_text();
	    except:
	        summary = None
	    if summary == None or summary == '':
	        summary = 'no valid NSURL here'
	    return summary
	return ''

def __lldb_init_module(debugger,dict):
	debugger.HandleCommand("type summary add -F NSURL.NSURL_SummaryProvider NSURL CFURLRef")
