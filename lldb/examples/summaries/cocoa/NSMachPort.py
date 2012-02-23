# summary provider for NSData
import lldb
import ctypes
import objc_runtime
import metrics

statistics = metrics.Metrics()
statistics.add_metric('invalid_isa')
statistics.add_metric('invalid_pointer')
statistics.add_metric('unknown_class')
statistics.add_metric('code_notrun')

# despite the similary to synthetic children providers, these classes are not
# trying to provide anything but the port number of an NSMachPort, so they need not
# obey the interface specification for synthetic children providers
class NSMachPortKnown_SummaryProvider:
	def adjust_for_architecture(self):
		self.lp64 = (self.valobj.GetTarget().GetProcess().GetAddressByteSize() == 8)
		self.is_little = (self.valobj.GetTarget().GetProcess().GetByteOrder() == lldb.eByteOrderLittle)
		self.pointer_size = self.valobj.GetTarget().GetProcess().GetAddressByteSize()

	def __init__(self, valobj):
		self.valobj = valobj;
		self.update();

	def update(self):
		self.adjust_for_architecture();
		self.id_type = self.valobj.GetType().GetBasicType(lldb.eBasicTypeObjCID)
		self.NSUInteger = self.valobj.GetType().GetBasicType(lldb.eBasicTypeUnsignedLong)

	# one pointer is the ISA
	# then we have one other internal pointer, plus
	# 4 bytes worth of flags. hence, these values
	def offset(self):
		if self.lp64:
			return 20
		else:
			return 12

	def port(self):
		vport = self.valobj.CreateChildAtOffset("port",
							self.offset(),
							self.NSUInteger)
		return vport.GetValueAsUnsigned(0)


class NSMachPortUnknown_SummaryProvider:
	def adjust_for_architecture(self):
		self.lp64 = (self.valobj.GetTarget().GetProcess().GetAddressByteSize() == 8)
		self.is_little = (self.valobj.GetTarget().GetProcess().GetByteOrder() == lldb.eByteOrderLittle)
		self.pointer_size = self.valobj.GetTarget().GetProcess().GetAddressByteSize()

	def __init__(self, valobj):
		self.valobj = valobj;
		self.update()

	def update(self):
		self.adjust_for_architecture();
		self.id_type = self.valobj.GetType().GetBasicType(lldb.eBasicTypeObjCID)

	def port(self):
		stream = lldb.SBStream()
		self.valobj.GetExpressionPath(stream)
		num_children_vo = self.valobj.CreateValueFromExpression("port","(int)[" + stream.GetData() + " machPort]");
		return num_children_vo.GetValueAsUnsigned(0)


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
	if name_string == 'NSMachPort':
		wrapper = NSMachPortKnown_SummaryProvider(valobj)
		statistics.metric_hit('code_notrun',valobj)
	else:
		wrapper = NSMachPortUnknown_SummaryProvider(valobj)
		statistics.metric_hit('unknown_class',str(valobj) + " seen as " + name_string)
	return wrapper;

def NSMachPort_SummaryProvider (valobj,dict):
	provider = GetSummary_Impl(valobj);
	if provider != None:
	    try:
	        summary = provider.port();
	    except:
	        summary = None
	    if summary == None:
	        summary = 'no valid mach port here'
	    return 'mach port: ' + str(summary)
	return ''

def __lldb_init_module(debugger,dict):
	debugger.HandleCommand("type summary add -F NSMachPort.NSMachPort_SummaryProvider NSMachPort")
